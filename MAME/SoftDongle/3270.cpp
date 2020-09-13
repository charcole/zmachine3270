#include <string.h>
#include "3270.h"

char EBCDICToASCII[256];
char ASCIIToEBCDIC[256];

static void Map(char ASCII, char EBCDIC)
{
	EBCDICToASCII[(uint8_t)EBCDIC] = ASCII;
	ASCIIToEBCDIC[(uint8_t)ASCII] = EBCDIC;
}

static void Map(char ASCIIStart, char ASCIIEnd, char EBCDIC)
{
	for (char ASCII = ASCIIStart; ASCII <= ASCIIEnd; ASCII++)
	{
		Map(ASCII, EBCDIC++);
	}
}

void BuildTextTables()
{
	memset(EBCDICToASCII, ' ', sizeof (EBCDICToASCII));
	memset(ASCIIToEBCDIC, 0x40, sizeof (ASCIIToEBCDIC));

	Map('a', 'i', 0x81);
	Map('j', 'r', 0x91);
	Map('s', 'z', 0xA2);
	Map('A', 'I', 0xC1);
	Map('J', 'R', 0xD1);
	Map('S', 'Z', 0xE2);
	Map('0', '9', 0xF0);
	Map('\n', 0x15);
	Map(' ', 0x40);
	Map('$', 0x4A); // Wikipedia says it should be cent but is $
	Map('.', 0x4B);
	Map('<', 0x4C);
	Map('(', 0x4D);
	Map('+', 0x4E);
	Map('|', 0x4F);
	Map('&', 0x50);
	Map('!', 0x5A);
	Map(0xA2, 0x5B); // Wikipedia says it should be $ but is pound (or filled square in US set)
	Map('*', 0x5C);
	Map(')', 0x5D);
	Map(';', 0x5E);
	Map(0xAC, 0x5F);
	Map('-', 0x60);
	Map('/', 0x61);
	Map(0xA6, 0x6A);
	Map(',', 0x6B);
	Map('%', 0x6C);
	Map('_', 0x6D);
	Map('>', 0x6E);
	Map('?', 0x6F);
	Map('`', 0x79);
	Map(':', 0x7A);
	Map('#', 0x7B);
	Map('@', 0x7C);
	Map('\'', 0x7D);
	Map('=', 0x7E);
	Map('"', 0x7F);
	Map(0xB1, 0x8F); // Doesn't seem to be in character set
	Map('~', 0xA1);
	Map('^', 0xB0); // Doesn't seem to be in character set
	Map('[', 0xBA); // Doesn't seem to be in character set
	Map(']', 0xBB); // Doesn't seem to be in character set
	Map('{', 0xC0);
	Map('}', 0xD0);
	Map('\\', 0xE0);
	
	Map('[', 0x8E); // Shouldn't be in character set but is [ (instead of at 0xBA)]
	Map(']', 0x8F); // Wikipedia says should be plus minus character but is ] (instead of at 0xBB)
}