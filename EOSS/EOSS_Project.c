/************************************************************************
*
* Module:			EOSS_Project.C
* Description:		Edge Of Space Science (EOSS) Project contributed by
*					BARC Jr.
* Line length:		120 characters [only if the length is longer than 80 chars]
* Functions:		See Below
*
* Date:				Authors:			Comments:
* 2 Jul 2011		Austin Schaller		Created
*			Nick ODell
*
************************************************************************/



/** Defines ************************************************************/

#define		MORSE_PIN						LATAbits.LATA0
#define		SPEAKER_PIN						LATAbits.LATA1

#define		MS_PER_TICK						120
#define		INTERRUPT_CLOCK_SETTING			5536			// See doc/timer_math.markdown
#define		MS_PER_CALLSIGN					10 * 60 * 1000	//10 minutes between callsigns, 60 seconds in a minute, 1000 milliseconds in a second.
#define		TICKS_PER_CALLSIGN				5000			//MS_PER_CALLSIGN/MS_PER_TICK
#define		PREFIX							16
#define		SUFFIX							17
#define		TERMINATOR						0xFF
#define		DATA_BYTES_PER_LINE				4

#define		CALLSIGN_SLOW_FACTOR			25
#define		ALTITUDE_SLOW_FACTOR			1

#define		FOSC		8000000
#define		BAUD 		9600

#define		TRUE		1
#define		FALSE		0

//** Include Files ******************************************************

#include <p18f4520.h>
#include <delays.h>
#include <stdio.h>
#include <stdlib.h>
#include <i2c.h>
#include <timers.h>
#include <math.h>		// Required for altitude measurement
#include "EOSS_Project.h"
#include "../BMP085/BMP085.c"		// IMPORTANT - Must include functions for BMP085 sensor!


#pragma config OSC=INTIO67, WDT=OFF, LVP=OFF, DEBUG=ON


//** Globals Variables **************************************************

// Last byte is the length
// These are automatically generated by a python script.
rom const unsigned char MorseCodeLib[21][DATA_BYTES_PER_LINE + 1] =
{
	{0x77, 0x77, 0x07, 0x00, 0x16},  // '0'
	{0xdd, 0xdd, 0x01, 0x00, 0x14},  // '1'
	{0x75, 0x77, 0x00, 0x00, 0x12},  // '2'
	{0xd5, 0x1d, 0x00, 0x00, 0x10},  // '3'
	{0x55, 0x07, 0x00, 0x00, 0x0e},  // '4'
	{0x55, 0x01, 0x00, 0x00, 0x0c},  // '5'
	{0x57, 0x05, 0x00, 0x00, 0x0e},  // '6'
	{0x77, 0x15, 0x00, 0x00, 0x10},  // '7'
	{0x77, 0x57, 0x00, 0x00, 0x12},  // '8'
	{0x77, 0x77, 0x01, 0x00, 0x14},  // '9'
	{0x1d, 0x00, 0x00, 0x00, 0x08},  // 'A'
	{0x57, 0x01, 0x00, 0x00, 0x0c},  // 'B'
	{0xd7, 0x05, 0x00, 0x00, 0x0e},  // 'C'
	{0x57, 0x00, 0x00, 0x00, 0x0a},  // 'D'
	{0x01, 0x00, 0x00, 0x00, 0x04},  // 'E'
	{0x75, 0x01, 0x00, 0x00, 0x0c},  // 'F'
	{0x1d, 0x5d, 0x71, 0x00, 0x1e},  // 'ALT '
	{0x55, 0xc0, 0x1d, 0x00, 0x1c},  // 'H M '
	{0xdd, 0x71, 0x77, 0x77, 0x20},  // 'W0DK/B'
	{0x5c, 0x71, 0x1d, 0x57, 0x20},  // 'W0DK/B'
	{0x17, 0x57, 0x01, 0x00, 0x14},  // 'W0DK/B'
};

unsigned char blockMorse[8];

// Timing stuff, all measured in ticks
unsigned short timeSinceCallsign = TICKS_PER_CALLSIGN + 1;	// This is so that the PIC transmits the callsign as soon as
															// it boots.
unsigned char nextReadingTime = 0;

// Schedule for morse code procedure
unsigned char schedule[32];
unsigned char txPos = 0;
unsigned char writePos = 0;
unsigned char skippy = 0;		// Used in conjunction with slowTimeLeft to skip morse transmission sometimes.
unsigned char slowTimeLeft = 0;	// Transmit 25 times slower (i.e. 3 second element length) if > 0. Measured in long elements

/************************************************************************
*
* Purpose:		To look at the schedule and find the scheduled bit.
* Passed:		0-255 depending on which bit you want to look at from the
*				schedule.
* Returned:		0-1 depending on the state of the bit.
* Note:			
* Date:			Author:			Comments:
* 16 Apr 2011	Nick ODell		Created
*
************************************************************************/

/*
The top 5 bits specify the byte and the lower 3 specify the bit. This
results in a more convinient representation of the data in the schedule
array.
*/
unsigned char getBitFromSchedule(unsigned char bitPos)
{
	// Return the state of the bit retreived
	return (schedule[bitPos >> 3] >> (bitPos & 0x07)) & 0x01;
}
	
/************************************************************************
*
* Purpose:		Called when the main loop deems it time to tranmit the next segment.
* Passed:		None
* Returned:		None
* Note:			
* Date:			Author:				Comments:
* 7 Mar 2011	Austin Schaller		Created
* 16 Apr 2011	Nick ODell			Gutted; renamed; replaced;
*
************************************************************************/
void stepMorse()
{
	unsigned char oneBit;
	if(slowTimeLeft > 0)
	{
		if(skippy == CALLSIGN_SLOW_FACTOR)
		{
			// Don't skip this time.
			skippy = 1;
			slowTimeLeft--;
		} else {
			// Skip!
			skippy++;
			return;
		}
	}
	else
	{
		if(skippy == ALTITUDE_SLOW_FACTOR)
		{
			// Don't skip this time.
			skippy = 1;
		} else {
			// Skip!
			skippy++;
			return;
		}
	}
	
	
	oneBit = getBitFromSchedule(txPos);
	
	MORSE_PIN = oneBit;
	//printf((const far rom char*) "Morse %d\r\n", oneBit);
	//printf((const far rom char*) "txPos: %d\r\n", txPos);
	//printf((const far rom char*) "writePos: %d\r\n", writePos);
	//printf((const far rom char*) "Byte: %d\r\n", schedule[txPos >> 3]);
	
	
	if((txPos & 0x07) == 0)
	{
		schedule[(txPos - 1) >> 3] = 0;		/*	Clear out the schedule behind you, so that bits from 256 ticks ago
												don't come back to haunt you.	*/
	}
	txPos++;
}

/************************************************************************
*
* Purpose:		Pushes the items given onto the schedule queue.
* Passed:		Morse code encoded as what's shown in MorseCodeLib. Terminate
* 				sequences with 0xFF.
* Returned:		None
* Date:			Author:			Comments:
* 16 Apr 2011	Nick O'Dell		Created
*
************************************************************************/
void scheduleMorse(unsigned char *morse)
{
	unsigned char i;
	unsigned char index, txBit;
	
	
	while((index = *morse++) != TERMINATOR)
	{
		unsigned char length = MorseCodeLib[index][DATA_BYTES_PER_LINE];
		printf((const far rom char*) "MorseCodeLib: %d\r\n", index);
		//printf((const far rom char*) "\r\n");
		for(i = 0; i < length; i++)
		{
			/*
			We get the bit from the morse code library, then OR it with
			the byte already in the schedule. Result: We write one element
			to the schedule.
			*/
			
			txBit = (MorseCodeLib[index][i >> 3] >> (i & 0x07)) & 0x01;
			//printf((const far rom char*) "%d", txBit);
			
			schedule[writePos >> 3] |= txBit << (writePos & 0x07);
			//printf((const far rom char*) "SByte: %d\r\n", schedule[writePos >> 3]);
			writePos++;
		}
		//printf((const far rom char*) "\r\n");
	}
}

/************************************************************************
*
* Purpose:		Determines the length of the block-morse passed to it
* Passed:		Morse code encoded as what's shown in MorseCodeLib. Terminate
* 				sequences with 0xFF.
*
* Returned:		The length of the morse sequence in elements.
*
* Date:		Author:				Comments:
* 17 Sep 2011	Nick O'Dell		Created
*
************************************************************************/

unsigned char getLengthOfMorse(unsigned char *morse)
{
	unsigned char length = 0;
	unsigned char index = 0;
	while((index = *morse++) != TERMINATOR)
	{
		length += MorseCodeLib[index][DATA_BYTES_PER_LINE];
	}
	return length;
}

/************************************************************************
*
* Purpose:		Pushes the callsign onto the schedule queue. This function
* expects that txPos == writePos
* Passed:		None
* Returned:		None
*
* Date:			Author:			Comments:
* 16 Apr 2011	Nick O'Dell		Created
*
************************************************************************/
void txCallsign()
{
	unsigned char morse[] = {18, 19, 20, TERMINATOR};
	unsigned char length = getLengthOfMorse(&morse[0]);
	
	//printf((const far rom char*) "txCallsign\r\n");

	slowTimeLeft = length;
	scheduleMorse(&morse[0]);
	//scheduleDump();
}

/************************************************************************
*
* Purpose:		Dumps the schedule to printf
* Passed:		None
* Returned:		None
*
* Date:			Author:			Comments:
* 16 Apr 2011	Nick O'Dell		Created
*
************************************************************************/
void scheduleDump()
{
	int i;
	for(i = 0; i < 32; i++)
	{
		printf((const far rom char*) "%d: %d\r\n", i, schedule[i]);
	}
}



/************************************************************************
*
* Purpose:		Configures USART module for TX operation
* Passed:		SPBRG, TXSTA, RCSTA
* Returned:		None
* Note:			Asynchronous Mode
*
* Date:		Author:					Comments:
* 20 Sep 2011	Austin Schaller		Created
*
************************************************************************/
void openTxUsart(void)
{
	// TX UART Configuration
	
	SPBRG = 12;				// Set baud = 9600
	TXSTAbits.SYNC = 0;		// Asynchronous mode
	RCSTAbits.SPEN = 1;		// Serial port enabled
	TXSTAbits.BRGH = 0;		// Low Speed
	TXSTAbits.TXEN = 1;		// Enable transmission
}



/************************************************************************
*
* Purpose:		Changes altitude into the morse library sequence
* Passed:		Altitude, Pointer to array
* Returned:		None
*
* Date:			Author:			Comments:
* 16 Apr 2011	Nick O'Dell		Created
*
************************************************************************/

//Sorta hacky, but oh well.
#define INSERT_IN_MORSE(item) *morsePointer++ = (item);

void formatAltitude(unsigned short alt, unsigned char *morsePointer)
{
	signed char i;
	unsigned char leading_zero = TRUE;
	unsigned char array_index = 0;
	unsigned short number;
	
	//printf((const far rom char*) "formatAltitude start\r\n");
	
	INSERT_IN_MORSE(PREFIX)
	
	// This counts down from 4 to 0 because the most significant digit comes first.
	for(i = 4; i >= 0; i--)
	{
		number = (unsigned short) alt / pow(10, i);
		number = number % 10;
		
		/*
		If the other digits so far have been zeros, and this
		is a zero, and this isn't the last digit, don't output
		this digit.
		*/
		
		if(leading_zero == TRUE && number == 0 && i != 0)
		{
			// Do nothing
		}
		else
		{
			INSERT_IN_MORSE(number);
			leading_zero = FALSE;
		}
	}
	
	INSERT_IN_MORSE(SUFFIX);			// The "H M" at the end of the sequence
	
	INSERT_IN_MORSE(TERMINATOR);		// Terminator
	
	//printf((const far rom char*) "formatAltitude end\r\n");
}

/************************************************************************
*
* Purpose:		Transmits characters
* Passed:		TXIF, TXREG
* Returned:			None
*
* Date:			Author:				Comments:
* 20 Sep 2011	Austin Schaller		Created
*
************************************************************************/
void txUsart(const rom char *data)
{
	char t;
	while(t = *data++)
	{
		while(!PIR1bits.TXIF);		// Ready for new character?
		TXREG = t;		// Send character
	}
}

/************************************************************************
*
* Purpose:		Set up timer0
* Passed:		None
* Returned:		None
*
************************************************************************/
void activateInterrupt(void)
{
	OpenTimer0(TIMER_INT_ON & T0_16BIT & T0_SOURCE_INT & T0_PS_1_4);
	INTCONbits.GIEH = 1; //enable interrupts
}

/************************************************************************
*
* Purpose:		Called during interrupt; resets timer and increments
* 				timeSinceCallsign.
* Passed:		None
* Returned:		None
*
************************************************************************/
#pragma code onInterrupt = 0x08
#pragma interrupt onInterrupt
void onInterrupt(void)
{
	if(INTCONbits.TMR0IF) {
		WriteTimer0(INTERRUPT_CLOCK_SETTING);
		timeSinceCallsign++;
		INTCONbits.TMR0IF = 0; // Clear the interrupt
		stepMorse();
		
		//printf((const far rom char*) "INTERRUPT\r\n");
		//MORSE_PIN = ~MORSE_PIN;
	}
}

/************************************************************************
*
* Purpose:		Checks whether one and two are within maxDist of each other.
* Passed:		None
* Returned:		None
*
************************************************************************/
unsigned char checkNear(unsigned char one, unsigned char two, unsigned char maxDist)
{
	signed short result;
	signed short maxDistTemp = maxDist;
	
	result = one;
	result -= two;
	
	//printf((const far rom char*) "result: %d\r\n", result);
	if(result < 0)
	{
		result = -result;
	}
	if(result <= maxDistTemp)
	{
		return TRUE;
	}
	else
	{
		return FALSE;
	}
}

/** Main Loop **********************************************************/
void main()
{
	int i, j;
	long temperature = 0;
	long pressure = 0;
	signed short altitude = 0;
	double temporary = 0;
	unsigned char length;
	unsigned char firstRun = TRUE;
	
	OSCCONbits.IRCF0=1;
	OSCCONbits.IRCF1=1;
	OSCCONbits.IRCF2=1;
	while(!OSCCONbits.IOFS);
	TRISA = 0x00;
	
	// Initialize I2C
	OpenI2C(MASTER, SLEW_OFF);
	
	// Initialize BMP085
	BMP085_Calibration();
	
	// Intialize SPI
	openTxUsart();
	
	
	// Erase Schedule
	for(i = 0; i < 32; i++)
	{
		schedule[i] = 0;
	}
	
	printf((const far rom char*) "\r\n=========================\r\n");
	printf((const far rom char*) "=========RESTART=========\r\n");
	printf((const far rom char*) "=========RESTART=========\r\n");
	printf((const far rom char*) "=========RESTART=========\r\n");
	printf((const far rom char*) "=========================\r\n");
	
	// Initialize Timer Interrupt
	activateInterrupt();					// Set up the timer
	WriteTimer0(INTERRUPT_CLOCK_SETTING);	// Set the timer
	timeSinceCallsign = 0; //TICKS_PER_CALLSIGN + 1;
	
	while(1)
	{
		if((timeSinceCallsign & 0x0F) == 0)
		{
			//printf((const far rom char*) "timeSinceCallsign: %d\r\n", timeSinceCallsign);
		}
		//printf((const far rom char*) "Loop\r\n");
		//printf((const far rom char*) "txPos: %d\r\n", txPos);
		//printf((const far rom char*) "writePos: %d\r\n", writePos);
		
		if(checkNear(txPos, writePos, \
			slowTimeLeft ? 1 : 25)) // Make sure you always have three seconds of morse left.
		{
			
			printf((const far rom char*) "Getting more morse\r\n");
			// Temperature & Pressure Measurements
			bmp085Convert(&temperature, &pressure);
			
			// Altitude Measurement
			temporary = (double) pressure / 101325;
			temporary = 1 - pow(temporary, 0.19029);
			
			// Will only work if temporary is positive.
			altitude = floor((44330 * temporary) + 0.5);
			printf((const far rom char*) "altitude: %d\r\n", altitude);
			
			formatAltitude(altitude, &blockMorse[0]);
			length = getLengthOfMorse(&blockMorse[0]);
			
			if((timeSinceCallsign + length + 25) > TICKS_PER_CALLSIGN)
			{
				// Make sure that we set the timing variables correctly
				nextReadingTime = nextReadingTime - (timeSinceCallsign & 0xFF);
				timeSinceCallsign = 0;
				
				if(firstRun)
				{
					nextReadingTime = 0;
					firstRun = FALSE;
				}
				
				//Bug: this starts the slow part ahead of time, meaning some of the morse is caught by it.
				
				// Transmit call sign
				txCallsign();
				printf((const far rom char*) "slowTimeLeft: %d\r\n", slowTimeLeft);
			}
			else
			{
				scheduleMorse(&blockMorse[0]);
			}
		}
		
		if((timeSinceCallsign & 0xFF) == nextReadingTime)
		{
			//printf((const far rom char*) "Getting a reading\r\n");
			//bmp085Convert(&temperature, &pressure);
			// Store temperature, pressure in I2C Memory
		}
		
		if(MORSE_PIN)
		{
			for(j = 100; j >= 0 && MORSE_PIN; j--)
			{
				SPEAKER_PIN = ~SPEAKER_PIN;
				Delay100TCYx(40);
			}
		}
	}
}
