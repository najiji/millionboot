/*
 * millionboot is a bootloader for ATMEL ATmega uC
 * millionboot is capable of receiving a firmware update over an unidirectional SPI channel
 * shared with a pool of identical systems.
 *
 *  Created on: 19.08.2015
 *      Author: najiji
 */


/*
 * WARNING: This code is currently configured for:
 * Atmega328p with bootloader size of 4kB.
 * YOU NEED TO CHANGE LINKER SETTINGS IN ORDER TO SUPPORT OTHER PLATFORMS!
 */

#include <avr/boot.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/delay.h>
#include <util/twi.h>
#include <avr/eeprom.h>
#include "io/uart.h"
#include "hex_parse.h"

#include <stdio.h>

/**
 * CONFIGURATION----------------------
 */
#define DEBUG 1
#define FIRMWARE_UPDATE_COMMAND 0xAA
#define INTEL_HEX_MAX_LINE_LENGTH 46

#define TIMER_ROUNDTRIPS_SECOND ((F_CPU/1024)/255)


/**
 * Global variables
 */
volatile uint8_t device_address = 0x00;
volatile uint8_t timeout_counter = TIMER_ROUNDTRIPS_SECOND;

/**
 * Jumps to 0x000 and therefore executes application software
 */
void start_application(void){
	//relocate interrupt table back to beginning of flash
	uint8_t temp = MCUCR;
	MCUCR = temp &~(1<<IVCE);
	MCUCR = temp &~(1<<IVSEL);

	//deactivate I2C
	TWAR = 0x00;
	TWCR = 0x00;

	_delay_ms(100);
	//jump to beginning of flash
	asm volatile ("jmp 0x000");
}


void set_timeout(){
	OCR0A = 0xFF; //set upper border for the counter
	TCCR0A |= (1<<WGM01);
	TCCR0B = (1<<CS00)|(1<<CS02); //enable prescaler 1024 and ctc mode
	TIMSK0 |= (1<<OCIE0A); //enable timer compare Interrupt
}


/**
 * Writes a page to memory
 */
void program_page (uint32_t page, uint8_t *buf)
{
    uint16_t i;
    uint8_t sreg;

    /* Disable interrupts */
    sreg = SREG;
    cli();

    eeprom_busy_wait ();

    boot_page_erase (page);
    boot_spm_busy_wait ();      /* Wait until the memory is erased. */

    for (i=0; i<SPM_PAGESIZE; i+=2)
    {
        /* Set up little-endian word. */
        uint16_t w = *buf++;
        w += (*buf++) << 8;

        boot_page_fill (page + i, w);
    }

    boot_page_write (page);     /* Store buffer in flash page.		*/
    boot_spm_busy_wait();       /* Wait until the memory is written.*/

    /* Reenable RWW-section again. We need this if we want to jump back */
    /* to the application after bootloading. */
    boot_rww_enable ();

    /* Re-enable interrupts (if they were ever enabled). */
    SREG = sreg;
}




/**
 * Bootloader entrance
 */
int main(){

	uint8_t hex_receive_buffer[INTEL_HEX_MAX_LINE_LENGTH];

	//Relocate interrupt vector table into boot section
	uint8_t temp = MCUCR;
	MCUCR = temp | (1<<IVCE);
	MCUCR = temp | (1<<IVSEL);

#if DEBUG
	uart_init(9600);
	uart_send("millionboot loaded.. \r\n");
	sei();
#endif

	//load device address from eeprom
	device_address = eeprom_read_byte(0x00);

	//configure i2c in slave mode
	TWAR = (device_address << 1)| (1<<TWGCE);
	TWCR = (1<<TWEA)|(1<<TWEN);
	TWCR &= ~(1<<TWINT);


	//set timeout
	set_timeout();

	//check if there is something on I2C or timeout expires
	while(!(TWCR & (1<<TWINT))){
		if(timeout_counter <= 0){
#if DEBUG
			uart_send("No data on I2C. Exiting.. \r\n");
#endif
			start_application();
		}
	}
	if(!(TWSR == TW_SR_GCALL_ACK)){
#if DEBUG
		uart_send("No general call on I2C. Exiting..  \r\n");
#endif
		TWCR = (1<<TWINT)|(1<<TWEN)|(1<<TWSTO)|(1<<TWEA);
		start_application();
	}

	//request next
	TWCR |= (1<<TWINT);


	//wait for data, or timeout
	while(!(TWCR & (1<<TWINT))){
		if(timeout_counter <= 0){
#if DEBUG
			uart_send("No command byte on I2C. Exiting.. \r\n");
#endif
			TWCR = (1<<TWINT)|(1<<TWEN)|(1<<TWSTO)|(1<<TWEA);
			start_application();
		}
	}

	if(!(TWSR == TW_SR_GCALL_DATA_ACK) || TWDR != FIRMWARE_UPDATE_COMMAND){
#if DEBUG
		uart_send("Wrong command byte. Exiting.. \r\n");
#endif
		TWCR = (1<<TWINT)|(1<<TWEN)|(1<<TWSTO)|(1<<TWEA);
		start_application();
	}

	TWCR = (1<<TWINT)|(1<<TWEN)|(1<<TWSTO)|(1<<TWEA);

	/**
	 * Entered firmware upgrade mode
	 */
#if DEBUG
	uart_send("entered firmware update mode.. \r\n");
#endif
	TWAR &= ~(1<<TWGCE);	//disable general call

	uint8_t upgrade_finished = 0;
	while(!upgrade_finished){
		//wait for beginning of next line transmission
		TWCR = (1<<TWINT)|(1<<TWEN)|(1<<TWSTO)|(1<<TWEA);
		while(!(TWCR & (1<<TWINT)));
		if(TWSR != TW_SR_SLA_ACK){
			TWCR = (1<<TWINT)|(1<<TWEN);
			continue;
		}

		//receive line
		uint8_t i;
		for(i=0; i<INTEL_HEX_MAX_LINE_LENGTH-1; i++){
			while(!(TWCR & (1<<TWINT)));
			if(TWSR == TW_SR_DATA_ACK){
				hex_receive_buffer[i] = TWDR;
				if(hex_receive_buffer[i] == '\r')
					TWCR = (1<<TWINT)|(1<<TWEN);
				else
					TWCR = (1<<TWINT)|(1<<TWEN)|(1<<TWEA);
			}
			//revieved whole line
			if(TWSR == TW_SR_DATA_NACK){
				break;
			}
		}

		uart_send(hex_receive_buffer);
		uart_send("\r\n");



	}

	while(1);
}

/*
 * Timeout occured -> switch to normal application
 */
ISR(TIMER0_COMPA_vect)
	{
		if(timeout_counter > 0)
			timeout_counter--;
		else
		{
			TIMSK0 &= ~(1<<OCIE0A); //disable compare interrupt
			TCCR0B = 0x00;			//disable timer 0
		}
	}
