// Note: This is a very old C port of a Java program (that I wrote for playing Zork on Livescribe pens)
//       Therefore does wierd things especially the &0xFFFF stuff as Java didn't have unsigned ints
//       Probably not good starting point for other projects but it does mostly support V3-V5 + V8 and Quetzal saves

#include <esp_system.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

#include "Screen.h"

typedef unsigned char		u8;
typedef unsigned short		u16;
typedef unsigned int		u32;

typedef char				s8;
typedef signed short		s16;
typedef signed int			s32;

#define TRUE	1
#define FALSE	0

#define MIN(a,b)		(((a)<(b))?(a):(b))
#define ASSERT(a) do { if (!(a)) { printf("Assert failed %s:%d\n", __FILE__, __LINE__); } } while(0);
#define ARRAY_SIZEOF(a) (sizeof(a)/sizeof((a)[0]))

typedef struct stack_s
{
	void *base;
	u32 size;
	u32 depth;
	u32 current;
} stack;

void stackInit(stack *s, void *base, u32 size, u32 depth)
{
	s->base=base;
	s->size=size;
	s->depth=depth;
	s->current=0;
}

int stackFull(stack *s)
{
	return s->current>=s->depth;
}

int stackEmpty(stack *s)
{
	return s->current==0;
}

int stackDepth(stack *s)
{
	return s->current;
}

void *stackPush(stack *s)
{
	void *ret=(u8*)s->base+s->size*s->current;
	ASSERT(!stackFull(s));
	s->current++;
	return ret;
}

void *stackPop(stack *s)
{
	ASSERT(!stackEmpty(s));
	s->current--;
	return (u8*)s->base+s->size*s->current;
}

void *stackPeek(stack *s)
{
	ASSERT(!stackEmpty(s));
	void *ret=(u8*)s->base+s->size*(s->current-1);
	return ret;
}
#define NUM_COLS 80
#define NUM_ROWS 24

int curWindow=0;
int splitLines=0;
int cursorX[3]={0,0,0};
int cursorY[3]={0,0,0};
int windowStart[3]={0,0,0};

void ioPrintChar(char c)
{
	ScreenPrintChar(c);
}

void ioSetCursor(int x, int y)
{
	ScreenSetCursor(x,y+windowStart[curWindow]);
}

void ioGetCursor(int* x, int* y)
{
	ScreenGetCursor(x,y);
	*y-=windowStart[curWindow];
}

void ioSplitWindow(int numLines)
{
	ASSERT(numLines>0);
	windowStart[0]=numLines+windowStart[1];
	ScreenSetNonScrollRows(windowStart[0]);
}

void ioUnsplitWindow()
{
	windowStart[0]=windowStart[1];
	ScreenSetNonScrollRows(windowStart[0]);
}

int ioSetWindow(int windowNum, int reset)
{
	int oldWindow=curWindow;
	ASSERT(windowNum>=0 && windowNum<=2);
	ioGetCursor(&cursorX[oldWindow],&cursorY[oldWindow]);
	curWindow=windowNum;
	if (reset)
	{
		cursorX[curWindow]=0;
		cursorY[curWindow]=0;
	}
	ioSetCursor(cursorX[curWindow],cursorY[curWindow]);
	return oldWindow;
}

void ioEraseWindow(int windowNum)
{
	int i;
	ioGetCursor(&cursorX[curWindow],&cursorY[curWindow]);
	switch (windowNum)
	{
	case 0:
		for (i=windowStart[0];i<NUM_ROWS;i++)
		{
			int x;
			ioSetCursor(0,i);
			for (x=0; x<NUM_COLS; x++)
			{
				ScreenPrintChar(' ');
			}
		}
		break;
	case 1:
	case 2:
		for (i=windowStart[windowNum];i<windowStart[windowNum-1];i++)
		{
			int x;
			ioSetCursor(0,i);
			for (x=0; x<NUM_COLS; x++)
			{
				ScreenPrintChar(' ');
			}
		}
		break;
	default:
		printf("Invalid erase window %d\n", windowNum);
		break;
	}
	ioSetCursor(cursorX[curWindow],cursorY[curWindow]);
}

void ioEraseToEndOfLine()
{
	int i;
	ioGetCursor(&cursorX[curWindow],&cursorY[curWindow]);
	for (i=cursorX[curWindow]; i<NUM_COLS; i++)
	{
		ScreenPrintChar(' ');
	}
	ioSetCursor(cursorX[curWindow],cursorY[curWindow]);
}

void ioSetStyleBits(int bits)
{
}

void ioClearStyleBits()
{
}

void ioReadInput(char* input, int size, int single)
{
	if (single)
		ScreenReadInputSingle(input, size);
	else
		ScreenReadInput(input, size);
}

unsigned int ioRandom()
{
	return esp_random();
}

void ioReset(int bHasStatus)
{
	curWindow=0;
	windowStart[2]=0;
	windowStart[1]=bHasStatus?1:0;
	windowStart[0]=windowStart[1];
	cursorX[0]=0;
	cursorX[1]=0;
	cursorX[2]=0;
	cursorY[0]=NUM_ROWS-1;
	cursorY[1]=0;
	cursorY[2]=0;
	ioEraseWindow(2);
	ioEraseWindow(0);
}

#define FROTZ_WATCHING 0 // Similar to Frotz's -a -A -o -O flags
#define MAX_TOKEN_LEN 24

enum
{
	SrcImmediate,
	SrcVariable
};

enum
{
	Form0OP,
	Form1OP,
	Form2OP,
	FormVAR,
	FormEXT
};

static int zeroOpStoreInstructions[]={};
static int zeroOpStoreInstructionsV4[]={0x05,0x06,0x09};
static int oneOpStoreInstructions[]={0x01,0x02,0x03,0x04,0x08,0x0E,0x0F};
static int oneOpStoreInstructionsV5[]={0x01,0x02,0x03,0x04,0x08,0x0E};
static int twoOpStoreInstructions[]={0x08,0x09,0x0F,0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19};
static int varOpStoreInstructions[]={0x00,0x07,0x0C,0x16,0x17,0x18};
static int varOpStoreInstructionsV5[]={0x00,0x04,0x07,0x0C,0x16,0x17,0x18};
static int extOpStoreInstructions[]={0x00,0x01,0x02,0x03,0x04,0x09,0x0A};

static int zeroOpBranchInstructions[]={0x05,0x06,0x0D,0x0F};
static int zeroOpBranchInstructionsV4[]={0x0D,0x0F};
static int oneOpBranchInstructions[]={0x00,0x01,0x02};
static int twoOpBranchInstructions[]={0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x0A};
static int varOpBranchInstructions[]={0x17,0x1F};
static int extOpBranchInstructions[]={};
	
static char alphabetLookup[3][27]={
	{ 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z' },
	{ 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z' },
	{ ' ', '\n','0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '.', ',', '!', '?', '_', '#', '\'','"', '/', '\\','-', ':', '(', ')' },
};

typedef struct ZOperand_s
{
	int value;
	int src;
} ZOperand;

typedef struct ZBranch_s
{
	int offset;
	int negate;
} ZBranch;

typedef struct ZInstruction_s
{
	int op;
	int form;
	int store;
	int numOps;
	ZBranch branch;
	ZOperand operands[8];
} ZInstruction;

typedef struct ZCallStack_s
{
	int returnAddr;
	int returnStore;
	char setArguments;
	char numLocals;
	s16 locals[15];
	int depth;
} ZCallStack;

typedef struct ZObject_s
{
	int addr;
	int propTable;
} ZObject;

typedef struct ZProperty_s
{
	int addr;
	int size;
	int bDefault;
} ZProperty;

typedef struct ZToken_s
{
	char token[MAX_TOKEN_LEN];
	int offset;
} ZToken;
	
typedef struct ZDictEntry_s
{
	int coded[6];
	int current;
} ZDictEntry;

typedef struct SaveStream_s
{
	uint8_t buffer[1024];
	uint8_t* ptr;
	uint8_t* end;
	int pos;
	FILE* f;

	int chunkSizeIdx;
	int chunkSize[32];
	int chunkStackIdx;
	uint8_t chunkStack[16];
} SaveStream;

typedef char byte;

static ZInstruction m_ins;
static int m_pc;
static int m_globalVariables;
static int m_abbrevTable;
static int m_objectTable;
static int m_dictionaryTable;
static int m_endOfDynamic;
static int m_version;
static int m_packedMultiplier;
static const byte *memory;
static s16 m_numberstack[256];
static stack m_stack;
static ZCallStack m_callstackcontents[96];
static stack m_callStack;
static SaveStream m_savestream;
static int m_memStreamPtr;
static int m_outputStream;
static char m_input[128];

#define DYNAMIC_CHUNK 1024
#define DYNAMIC(x) dynamicChunks[x/DYNAMIC_CHUNK][x&(DYNAMIC_CHUNK-1)]
static byte *dynamicChunks[64];

byte ReadMemory(int Address)
{
	if (Address < m_endOfDynamic)
	{
		return DYNAMIC(Address);
	}
	return memory[Address];
}

void SetMemory(int Address, byte Value)
{
	ASSERT(Address < m_endOfDynamic);
	DYNAMIC(Address) = Value;
}

int makeS16(int msb, int lsb)
{
	int ret=(msb<<8)+lsb;
	if ((ret&0x8000)!=0)
	{
		ret+=-0x10000;
	}
	return ret;
}

int makeU16(int msb, int lsb)
{
	return (msb<<8)+lsb;
}

int readBytePC()
{
	return ReadMemory(m_pc++)&0xFF;
}

int readS16PC()
{
	int msb=readBytePC();
	int lsb=readBytePC();
	return makeS16(msb, lsb);
}

int readVariable(int var)
{
	if (var==0)
	{
		return *(s16*)stackPop(&m_stack);
	}
	if (var<16)
	{
		return ((ZCallStack*)stackPeek(&m_callStack))->locals[var-1];
	}
	int off=2*(var-16);
	off+=m_globalVariables;
	return makeS16(ReadMemory(off)&0xFF, ReadMemory(off+1)&0xFF); 
}

void setVariable(int var, int value)
{
	value&=0xFFFF;
	if ((value&0x8000)!=0)
	{
		value+=-0x10000;
	}
	if (var==0)
	{
		*(s16*)stackPush(&m_stack)=value;
		return;
	}
	if (var<16)
	{
		((ZCallStack*)stackPeek(&m_callStack))->locals[var-1]=value;
		return;
	}
	int off=2*(var-16);
	off+=m_globalVariables;
	SetMemory(off+0, (byte)((value&0xFF00)>>8));
	SetMemory(off+1, (byte)((value&0x00FF)>>0)); 
}

void setVariableIndirect(int var, int value)
{
	if (var==0)
	{
		stackPop(&m_stack);
	}
	setVariable(var, value);
}

int readVariableIndirect(int var)
{
	int ret=readVariable(var);
	if (var==0)
	{
		setVariable(var, ret);
	}
	return ret;
}

ZObject getObject(int id)
{
	ZObject ret;
	if (m_version<=3)
	{
		ret.addr=m_objectTable+2*31+9*(id-1);
		ret.propTable=makeU16(ReadMemory(ret.addr+7)&0xFF, ReadMemory(ret.addr+8)&0xFF);
	}
	else
	{
		ret.addr=m_objectTable+2*63+14*(id-1);
		ret.propTable=makeU16(ReadMemory(ret.addr+12)&0xFF, ReadMemory(ret.addr+13)&0xFF);
	}
	return ret;
}

int zobjectGetParent(ZObject obj)
{
	if (m_version<=3)
	{
		return ReadMemory(obj.addr+4)&0xFF;
	}
	return makeU16(ReadMemory(obj.addr+6)&0xFF, ReadMemory(obj.addr+7)&0xFF);
}

int zobjectGetSibling(ZObject obj)
{
	if (m_version<=3)
	{
		return ReadMemory(obj.addr+5)&0xFF;
	}
	return makeU16(ReadMemory(obj.addr+8)&0xFF, ReadMemory(obj.addr+9)&0xFF);
}

int zobjectGetChild(ZObject obj)
{
	if (m_version<=3)
	{
		return ReadMemory(obj.addr+6)&0xFF;
	}
	return makeU16(ReadMemory(obj.addr+10)&0xFF, ReadMemory(obj.addr+11)&0xFF);
}

void zobjectSetParent(ZObject obj, int id)
{
	if (m_version<=3)
	{
		SetMemory(obj.addr+4,id);
	}
	else
	{
		SetMemory(obj.addr+6,(id/256)&0xFF);
		SetMemory(obj.addr+7,id&0xFF);
	}
}

void zobjectSetSibling(ZObject obj, int id)
{
	if (m_version<=3)
	{
		SetMemory(obj.addr+5,id);
	}
	else
	{
		SetMemory(obj.addr+8,(id/256)&0xFF);
		SetMemory(obj.addr+9,id&0xFF);
	}
}

void zobjectSetChild(ZObject obj, int id)
{
	if (m_version<=3)
	{
		SetMemory(obj.addr+6,id);
	}
	else
	{
		SetMemory(obj.addr+10,(id/256)&0xFF);
		SetMemory(obj.addr+11,id&0xFF);
	}
}

ZProperty getProperty(ZObject obj, int id)
{
	ZProperty ret;
	int address=obj.propTable;
	int textLen=ReadMemory(address++)&0xFF;
	address+=textLen*2;
	while (ReadMemory(address)!=0)
	{
		int sizeId=ReadMemory(address++)&0xFF;
		int size, propId;
		if (m_version<=3)
		{
			size=1+(sizeId>>5);
			propId=sizeId&31;
		}
		else
		{
			propId=sizeId&63;
			if (sizeId&0x80)
			{
				size=(ReadMemory(address++)&0xFF)&63;
				if (size==0)
					size=64;
			}
			else
			{
				size=(sizeId&0x40)?2:1;
			}
		}
		if (propId==id)
		{
			ret.addr=address;
			ret.size=size;
			ret.bDefault=FALSE;
			return ret;
		}
		address+=size;
	}
	ret.addr=(m_objectTable+(id-1)*2)&0xFFFF;
	ret.size=2;
	ret.bDefault=TRUE;
	return ret;
}

void returnRoutine(int value)
{
	ZCallStack *cs=stackPop(&m_callStack);
	while (cs->depth<stackDepth(&m_stack))
	{
		readVariable(0);
	}
	if (cs->returnStore!=-1)
	{
		setVariable(cs->returnStore, value);
	}
	m_pc=cs->returnAddr;
}

void doBranch(int cond, ZBranch branch)
{
	if (branch.negate)
	{
		cond=!cond;
	}
	if (cond)
	{
		if (branch.offset==0)
			returnRoutine(0);
		else if (branch.offset==1)
			returnRoutine(1);
		else
			m_pc+=branch.offset-2;
	}
}

void printToStream(char Value)
{
	if (m_memStreamPtr && Value!=0)
	{
		int Length = makeU16(ReadMemory(m_memStreamPtr)&0xFF,ReadMemory(m_memStreamPtr+1)&0xFF);
		Length++;
		SetMemory(m_memStreamPtr+0,Length>>8);
		SetMemory(m_memStreamPtr+1,Length&0xFF);
		if (Value=='\n')
			Value='\r';
		SetMemory(m_memStreamPtr+1+Length,Value);
	}
	else if (m_outputStream)
	{
		ioPrintChar(Value);
	}
}

void readOperand(int operandType)
{
	if (operandType==3) //omitted
	{
		return;
	}
	ZOperand *operand = &m_ins.operands[m_ins.numOps++];
	switch (operandType)
	{
		case 0: // long constant
			operand->value = readS16PC();
			operand->src = SrcImmediate;
			break;
		case 1: // small constant
			operand->value = readBytePC();
			operand->src = SrcImmediate;
			break;
		case 2: // variable
			operand->value = readVariable(readBytePC());
			operand->src = SrcVariable;
			break;
	}
}

void readShortForm(int opcode)
{
	int operand=(opcode>>4)&3;
	int op=opcode&15;
	m_ins.op=op;
	if (operand==3)
		m_ins.form=Form0OP;
	else
		m_ins.form=Form1OP;
	readOperand(operand);
}

void readLongForm(int opcode)
{
	int op=opcode&31;
	m_ins.op=op;
	m_ins.form=Form2OP;
	readOperand(((opcode&(1<<6))!=0)?2:1);
	readOperand(((opcode&(1<<5))!=0)?2:1);
}

void readVariableForm(int opcode)
{
	int op=opcode&31;
	int operandTypes=readBytePC();
	int operandTypes2=0xFF;
	int i;
	m_ins.op=op;
	if ((opcode&0xF0)>=0xE0)
		m_ins.form=FormVAR;
	else
		m_ins.form=Form2OP;
	if (m_ins.form==FormVAR && (op==0xC || op==0x1A))
		operandTypes2=readBytePC();
	for (i=3; i>=0; i--)
		readOperand((operandTypes>>(2*i))&3);
	for (i=3; i>=0; i--)
		readOperand((operandTypes2>>(2*i))&3);
}

void readExtendedForm()
{
	int op=readBytePC();
	int operandTypes=readBytePC();
	int i;
	m_ins.op=op;
	m_ins.form=FormEXT;
	for (i=3; i>=0; i--)
	{
		readOperand((operandTypes>>(2*i))&3);
	}
}

int readStoreInstruction(int *match, int length, int op)
{
	int i;
	for (i=0; i<length; i++)
	{
		if (op==match[i])
		{
			return readBytePC();
		}
	}
	return -1;
}

ZBranch readBranchInstruction(int *match, int length, int op)
{
	ZBranch ret;
	int i;
	for (i=0; i<length; i++)
	{
		if (op==match[i])
		{
			int branch1=readBytePC();
			if ((branch1&(1<<6))==0)
			{
				int branch2=readBytePC();
				int offset=((branch1&63)<<8)+branch2;
				if ((offset&(1<<13))!=0)
				{
					offset+=-(1<<14);
				}
				ret.offset=offset;
				ret.negate=((branch1&0x80)==0);
				return ret;
			}
			else
			{
				ret.offset=branch1&63;
				ret.negate=((branch1&0x80)==0);
				return ret;
			}
		}
	}
	ret.offset=0;
	ret.negate=FALSE;
	return ret;
}

void callRoutine(int address, int returnStore, int setOperands)
{
	if (address==0)
	{
		if (returnStore>=0)
			setVariable(returnStore, 0);
	}
	else
	{
		int numLocals=ReadMemory(address++)%0xFF;
		int i;
		ZCallStack cs;
		cs.returnAddr=m_pc;
		cs.returnStore=returnStore;
		cs.setArguments=(setOperands?(1<<(m_ins.numOps-1))-1:0);
		cs.numLocals=numLocals;
		if (m_version>=5)
		{
			memset(cs.locals, 0, cs.numLocals*sizeof(cs.locals[0]));
		}
		else
		{
			for (i=0; i<numLocals; i++)
			{
				cs.locals[i]=makeS16(ReadMemory(address)&0xFF, ReadMemory(address+1)&0xFF);
				address+=2;
			}
		}
		if (setOperands)
		{
			for (i=0; i<m_ins.numOps-1; i++)
			{
				cs.locals[i]=m_ins.operands[i+1].value;
			}
		}
		cs.depth=stackDepth(&m_stack);
		m_pc=address;
		*(ZCallStack*)stackPush(&m_callStack)=cs;
	}
}

void displayState()
{
	int i;
	printf("Next PC:%x\n", m_pc);
	printf("Form:%x Opcode:%x\n", m_ins.form, m_ins.op);
	printf("Num operands: %d\n", m_ins.numOps);
	for (i=0; i<m_ins.numOps; i++)
	{
		printf("Value:%d Src:%d\n", m_ins.operands[i].value, m_ins.operands[i].src); 
	}
	printf("Store:%d Branch:%d %s\n", m_ins.store, m_ins.branch.offset, (m_ins.branch.negate?" Negated":" Normal"));
}

void dumpCurrentInstruction()
{
	int i;
	printf("Form:%x Opcode:%x\n", m_ins.form, m_ins.op);
	for (i=0; i<m_ins.numOps; i++)
	{ 
		printf("Arg:%d Value:%d\n", i, (m_ins.operands[i].value&0xFFFF));
	}
}

void haltInstruction()
{
	printf("\n\nUnimplemented instruction!\n");
	displayState();
	exit(1);
}

void illegalInstruction()
{
	printf("\n\nIllegal instruction!\n");
	displayState();
	exit(1);
}

int printText(int address)
{
	int pair1=0, pair2=0;
	int alphabet=0;
	int characters[3];
	int abbrNext=FALSE;
	int longNext=0;
	int longChar=0;
	int abbrChar=0;
	while ((pair1&0x80)==0)
	{
		int i;
		pair1=ReadMemory(address++)&0xFF;
		pair2=ReadMemory(address++)&0xFF;
		characters[0]=(pair1&0x7C)>>2;
		characters[1]=((pair1&3)<<3) + ((pair2&0xE0)>>5);
		characters[2]=pair2&0x1F;
		for (i=0; i<3; i++)
		{
			if (longNext>0)
			{
				longChar<<=5;
				longChar&=0x3FF;
				longChar|=characters[i];
				longNext--;
				if (longNext==0)
				{
					printToStream((char)longChar);
				}
			}
			else if (!abbrNext)
			{
				if (characters[i]==6 && alphabet==2)
				{
					longNext=2;
					alphabet=0;
				}
				else if (characters[i]>=6)
				{
					characters[i]-=6;
					printToStream(alphabetLookup[alphabet][characters[i]]);
					alphabet=0;
				}
				else if (characters[i]==4)
				{
					alphabet=1;
				}
				else if (characters[i]==5)
				{
					alphabet=2;
				}
				else if (characters[i]==0)
				{
					printToStream(' ');
				}
				else
				{
					abbrChar=characters[i];
					abbrNext=TRUE;
				}
			}
			else
			{
				int idx=32*(abbrChar-1)+characters[i];
				int abbrevTable=m_abbrevTable+2*idx;
				int abbrevAddress=makeU16(ReadMemory(abbrevTable)&0xFF, ReadMemory(abbrevTable+1)&0xFF);
				printText(2*abbrevAddress);
				abbrNext=FALSE;
			}
		}
	}
	return address;
}

void removeObject(int childId)
{
	ZObject child=getObject(childId);
	int parentId=zobjectGetParent(child);
	if (parentId!=0)
	{
		ZObject parent=getObject(parentId);
		if (zobjectGetChild(parent)==childId)
		{
			zobjectSetChild(parent, zobjectGetSibling(child));
		}
		else
		{
			int siblingId=zobjectGetChild(parent);
			while (siblingId!=0)
			{
				ZObject sibling=getObject(siblingId);
				int nextSiblingId=zobjectGetSibling(sibling);
				if (nextSiblingId==childId)
				{
					zobjectSetSibling(sibling, zobjectGetSibling(child));
					break;
				}
				siblingId=nextSiblingId;
			}
			if (siblingId==0)
			{
				illegalInstruction();
			}
		}
		zobjectSetParent(child,0);
		zobjectSetSibling(child,0);
	}
}

void addChild(int parentId, int childId)
{
	ZObject child=getObject(childId);
	ZObject parent=getObject(parentId);
	zobjectSetSibling(child, zobjectGetChild(parent));
	zobjectSetParent(child, parentId);
	zobjectSetChild(parent, childId);
}

void zDictInit(ZDictEntry *entry)
{
	entry->current=0;
	entry->coded[0]=0;
	entry->coded[1]=0;
	entry->coded[2]=(m_version>3?0:0x80);
	entry->coded[3]=0;
	entry->coded[4]=(m_version>3?0x80:0);
	entry->coded[5]=0;
}

void zDictAddCharacter(ZDictEntry *entry, int code)
{
	code&=31;
	switch (entry->current)
	{
		case 0: entry->coded[0]|=code<<2; break;
		case 1: entry->coded[0]|=code>>3; entry->coded[1]|=(code<<5)&0xFF; break;
		case 2: entry->coded[1]|=code; break;
		case 3: entry->coded[2]|=code<<2; break;
		case 4: entry->coded[2]|=code>>3; entry->coded[3]|=(code<<5)&0xFF; break;
		case 5: entry->coded[3]|=code; break;
		case 6: entry->coded[4]|=code<<2; break;
		case 7: entry->coded[4]|=code>>3; entry->coded[5]|=(code<<5)&0xFF; break;
		case 8: entry->coded[5]|=code; break;
	}
	entry->current++;
}

ZDictEntry encodeToken(char* token)
{
	ZDictEntry ret;
	int tokenLen=strlen(token);
	int t;
	zDictInit(&ret);
	for (t=0; t<tokenLen; t++)
	{
		char curChar = token[t];
		int alphabet=-1;
		int code=-1;
		int a;
		for (a=0; a<3 && alphabet==-1; a++)
		{
			int i;
			for (i=0; i<27; i++)
			{
				if (curChar == alphabetLookup[a][i])
				{
					alphabet=a;
					code=i;
					break;
				}
			}
		}
		if (alphabet==-1)
		{
			zDictAddCharacter(&ret, 5);
			zDictAddCharacter(&ret, 6);
			zDictAddCharacter(&ret, curChar>>5);
			zDictAddCharacter(&ret, curChar&31);
		}
		else
		{
			if (alphabet>0)
			{
				int shift=alphabet+3;
				zDictAddCharacter(&ret, shift);
			}
			zDictAddCharacter(&ret, code+6);
		}
	}
	for (t=0; t<9; t++) // pad
	{
		zDictAddCharacter(&ret, 5);
	}
	return ret;
}

int getDictionaryAddress(char* token, int dictionary)
{
	int entryLength = ReadMemory(dictionary++)&0xFF;
	int numEntries = abs(makeS16(ReadMemory(dictionary+0)&0xFF, ReadMemory(dictionary+1)&0xFF));
	ZDictEntry zde = encodeToken(token);
	int i;
	dictionary+=2;
	for (i=0; i<numEntries; i++)
	{
		if (zde.coded[0]==(ReadMemory(dictionary+0)&0xFF) && zde.coded[1]==(ReadMemory(dictionary+1)&0xFF) &&
			zde.coded[2]==(ReadMemory(dictionary+2)&0xFF) && zde.coded[3]==(ReadMemory(dictionary+3)&0xFF))
		{
			if (m_version<=3 || (zde.coded[4]==(ReadMemory(dictionary+4)&0xFF) && zde.coded[5]==(ReadMemory(dictionary+5)&0xFF)))
			{
				return dictionary;
			}
		}
		dictionary+=entryLength;
	}
	return 0;
}

void lexicalAnalysis(char* input, int parseBuffer, int dictionaryAddress, int ignoreUnknown)
{
	static ZToken tokens[64];
	static char seps[32];
	int numTokens=0;
	int numSeperators=ReadMemory(dictionaryAddress++);
	char *current=input;
	char *end=input+strlen(current);
	int i;
	int maxEntries=ReadMemory(parseBuffer++)&0xFF;
	int parseReturn=parseBuffer++;
	ASSERT(maxEntries <= ARRAY_SIZEOF(tokens));
	ASSERT(numSeperators <= ARRAY_SIZEOF(seps));
	for (i=0; i<numSeperators; i++)
	{
		seps[i]=(char)ReadMemory(dictionaryAddress++);
	}
	while (current!=end)
	{
		char *space=strchr(current, ' ');
		char *min=end;
		int sepFound=FALSE;
		int tokLen;
		if (space==current)
		{
			current++;
			continue;
		}
		else if (space)
		{
			min=space;
		}
		for (i=0; i<numSeperators; i++)
		{
			char *sep=strchr(current, seps[i]);
			if (sep==current)
			{
				tokens[numTokens].offset=current-input;
				tokens[numTokens].token[0]=*current;
				tokens[numTokens].token[1]='\0';
				numTokens++;
				current++;
				sepFound=TRUE;
				break;
			}
			else if (sep && sep<min)
			{
				min=sep;
			}
		}
		if (sepFound)
		{
			continue;
		}

		tokens[numTokens].offset=(int)(current-input);
		tokLen=MIN(min-current, MAX_TOKEN_LEN-1);
		strncpy(tokens[numTokens].token, current, tokLen);
		tokens[numTokens].token[tokLen]='\0';
		numTokens++;
		current=min;
	}

	for (i=0; i<numTokens && i<maxEntries; i++)
	{
		int outAddress=getDictionaryAddress(tokens[i].token, dictionaryAddress);
		if (!ignoreUnknown || outAddress!=0)
		{
			SetMemory(parseBuffer++, (byte)((outAddress>>8)&0xFF));
			SetMemory(parseBuffer++, (byte)((outAddress>>0)&0xFF));
			SetMemory(parseBuffer++, (byte)strlen(tokens[i].token));
			SetMemory(parseBuffer++, (byte)(tokens[i].offset+(m_version>=5?2:1)));
		}
		else
		{
			parseBuffer+=4;
		}
	}

	SetMemory(parseReturn,(byte)MIN(maxEntries, numTokens));
}

void updateStatus()
{
	if (m_version<=3 && (ReadMemory(1) & (1<<4)) == 0)
	{
		int objectId=readVariable(16);
		int score=readVariable(17);
		int turns=readVariable(18);
		ZObject room=getObject(objectId);
		int statusX,statusY;
		char scoreString[32];
		char* scorePtr=scoreString;
		int oldWindow=ioSetWindow(2, TRUE);
		ioEraseWindow(2);
		ioSetStyleBits(1);
		ioPrintChar(' ');
		ASSERT(m_memStreamPtr==0);
		printText(room.propTable+1);
		ioGetCursor(&statusX,&statusY);
		while (statusX<68)
		{
			ioPrintChar(' ');
			statusX++;
		}
		if (ReadMemory(1)&(1<<1)) // Time game
		{
			sprintf(scoreString, "Time: %02d:%02d", score, turns);
		}
		else
		{
			if (score>=0)
				sprintf(scoreString, "Score: %04d", score);
			else
				sprintf(scoreString, "Score: %03d", score);
		}
		while (*scorePtr)
		{
			ioPrintChar(*scorePtr++);
		}
		ioPrintChar(' ');
		ioClearStyleBits();
		ioSetWindow(oldWindow, FALSE);
	}
}

void saveResetStream(SaveStream* stream)
{
	stream->ptr=stream->buffer;
	stream->end=stream->ptr+sizeof(stream->buffer);
	stream->pos=0;
	stream->f=NULL;
	stream->chunkSizeIdx=0;
	stream->chunkStackIdx=0;
}

void saveFlush(SaveStream* stream)
{
	if (stream->f)
	{
		fwrite(stream->buffer,1,stream->ptr-stream->buffer,stream->f);
	}
	stream->ptr=stream->buffer;
}

void saveByte(SaveStream* stream, uint8_t data)
{
	*(stream->ptr++)=data;
	stream->pos++;
	if (stream->ptr>=stream->end)
	{
		saveFlush(stream);
	}
}

void saveDynamicMemory(SaveStream* stream)
{
	int ZeroRun = 0;
	for (int i=0; i<m_endOfDynamic; i++)
	{
		uint8_t Diff = memory[i] ^ DYNAMIC(i);
		if (Diff == 0)
		{
			ZeroRun++;
		}
		else
		{
			while (ZeroRun > 0)
			{
				int Step = ZeroRun;
				if (Step > 256)
				{
					Step = 256;
				}
				saveByte(stream, 0);
				saveByte(stream, Step - 1);
				ZeroRun -= Step;
			}
			saveByte(stream, Diff);
		}
	}
}

void saveStacks(SaveStream* stream)
{
	int NumStacks = stackDepth(&m_callStack);
	for (int i=0; i<NumStacks; i++)
	{
		ZCallStack* Callstack = &m_callstackcontents[i];
		int NextNumberStackDepth = (i + 1) < NumStacks ? m_callstackcontents[i+1].depth : stackDepth(&m_stack);
		saveByte(stream, Callstack->returnAddr >> 16);
		saveByte(stream, Callstack->returnAddr >> 8);
		saveByte(stream, Callstack->returnAddr);
		saveByte(stream, Callstack->numLocals & 0x0F);
		saveByte(stream, Callstack->returnStore);
		saveByte(stream, Callstack->setArguments);
		saveByte(stream, (NextNumberStackDepth - Callstack->depth) >> 8);
		saveByte(stream, (NextNumberStackDepth - Callstack->depth));
		for (int s=0; s<Callstack->numLocals; s++)
		{
			saveByte(stream, Callstack->locals[s] >> 8);
			saveByte(stream, Callstack->locals[s]);
		}
		for (int s=Callstack->depth; s<NextNumberStackDepth; s++)
		{
			saveByte(stream, m_numberstack[s] >> 8);
			saveByte(stream, m_numberstack[s]);
		}
	}
}

void saveHeader(SaveStream* stream)
{
	// Seems weird but stored PC points at the branch part of the instruction
	int storePC = m_pc - 1;
	if (m_ins.branch.offset < 0 || m_ins.branch.offset > 63)
	{
		storePC--;
	}
	saveByte(stream, ReadMemory(0x02));
	saveByte(stream, ReadMemory(0x03));
	saveByte(stream, ReadMemory(0x12));
	saveByte(stream, ReadMemory(0x13));
	saveByte(stream, ReadMemory(0x14));
	saveByte(stream, ReadMemory(0x15));
	saveByte(stream, ReadMemory(0x16));
	saveByte(stream, ReadMemory(0x17));
	saveByte(stream, ReadMemory(0x1C));
	saveByte(stream, ReadMemory(0x1D));
	saveByte(stream, storePC >> 16);
	saveByte(stream, storePC >> 8);
	saveByte(stream, storePC);
}

void saveChunkPush(SaveStream* stream, char A, char B, char C, char D)
{
	int chunkSize = stream->chunkSize[stream->chunkSizeIdx];
	saveByte(stream, A);
	saveByte(stream, B);
	saveByte(stream, C);
	saveByte(stream, D);
	saveByte(stream, chunkSize>>24);
	saveByte(stream, chunkSize>>16);
	saveByte(stream, chunkSize>>8);
	saveByte(stream, chunkSize);
	stream->chunkStack[stream->chunkStackIdx++]=stream->chunkSizeIdx;
	stream->chunkSize[stream->chunkSizeIdx++]=stream->pos;
}

void saveChunkPop(SaveStream* stream)
{
	int sizeIndex=stream->chunkStack[--stream->chunkStackIdx];
	int chunkSize=stream->pos-stream->chunkSize[sizeIndex];
	stream->chunkSize[sizeIndex]=chunkSize;
	if (chunkSize&1)
	{
		saveByte(stream,0);
	}
}

void saveState(SaveStream* stream)
{
	saveChunkPush(stream,'F','O','R','M');
	saveByte(stream, 'I');
	saveByte(stream, 'F');
	saveByte(stream, 'Z');
	saveByte(stream, 'S');

	{
		saveChunkPush(stream,'I','F','h','d');
		saveHeader(stream);
		saveChunkPop(stream);

		saveChunkPush(stream,'C','M','e','m');
		saveDynamicMemory(stream);
		saveChunkPop(stream);

		saveChunkPush(stream,'S','t','k','s');
		saveStacks(stream);
		saveChunkPop(stream);
	}
	
	saveChunkPop(stream);
	saveFlush(stream);
}

void readFromDisk(SaveStream* stream)
{
	stream->end=stream->buffer+fread(stream->buffer,1,sizeof(stream->buffer),stream->f);
	stream->ptr=stream->buffer;
}

int readHasData(SaveStream* stream)
{
	return (stream->ptr<stream->end);
}

uint8_t readByte(SaveStream* stream)
{
	uint8_t ret;
	ret=*(stream->ptr++);
	stream->pos++;
	if (stream->ptr>=stream->end)
	{
		readFromDisk(stream);
	}
	return ret;
}

int restoreState(SaveStream* stream)
{
	char ChunkId[4];
	int bFoundHeader=FALSE;
	int bFoundMem=FALSE;
	int bFoundStacks=FALSE;
	int ChunkSize;
	int i;
	readFromDisk(stream);
	while (readHasData(stream))
	{
		int NextChunk;
		int ChunkEnd;
		ChunkId[0]=readByte(stream);
		ChunkId[1]=readByte(stream);
		ChunkId[2]=readByte(stream);
		ChunkId[3]=readByte(stream);
		ChunkSize=readByte(stream)<<24;
		ChunkSize+=readByte(stream)<<16;
		ChunkSize+=readByte(stream)<<8;
		ChunkSize+=readByte(stream);
		ChunkEnd=stream->pos+ChunkSize;
		NextChunk=stream->pos+((ChunkSize+1)&~1);
		if (ChunkId[0]=='F' && ChunkId[1]=='O' && ChunkId[2]=='R' && ChunkId[3]=='M')
		{
			ChunkId[0]=readByte(stream);
			ChunkId[1]=readByte(stream);
			ChunkId[2]=readByte(stream);
			ChunkId[3]=readByte(stream);
			if (ChunkId[0]=='I' && ChunkId[1]=='F' && ChunkId[2]=='Z' && ChunkId[3]=='S')
			{
				continue; // Want to read inner chunks so don't skip
			}	
		}
		else if (ChunkId[0]=='I' && ChunkId[1]=='F' && ChunkId[2]=='h' && ChunkId[3]=='d')
		{
			// release number, serial number and checksum
			int Locations[]={0x02, 0x03, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x1C, 0x1D};
			for (i=0; i<ARRAY_SIZEOF(Locations); i++)
			{
				if ((unsigned char)memory[Locations[i]]!=readByte(stream))
				{
					return FALSE;
				}
			}
			m_pc=readByte(stream)<<16;
			m_pc+=readByte(stream)<<8;
			m_pc+=readByte(stream);
			bFoundHeader=TRUE;
		}
		else if (ChunkId[0]=='C' && ChunkId[1]=='M' && ChunkId[2]=='e' && ChunkId[3]=='m')
		{
			int i;
			for (i=0; i<m_endOfDynamic; i++)
			{
				DYNAMIC(i) = memory[i];
			}
			i=0;
			while (stream->pos<ChunkEnd)
			{
				unsigned char Value=readByte(stream);
				if (Value==0x00)
				{
					int Length=readByte(stream)+1;
					i+=Length;
				}
				else
				{
					DYNAMIC(i) ^= Value;
					i++;
				}
			}
			bFoundMem=TRUE;
		}
		else if (ChunkId[0]=='U' && ChunkId[1]=='M' && ChunkId[2]=='e' && ChunkId[3]=='m')
		{
			int i;
			for (i=0; i<m_endOfDynamic; i++)
			{
				DYNAMIC(i) = memory[i];
			}
			i=0;
			while (stream->pos<ChunkEnd)
			{
				DYNAMIC(i) = readByte(stream);
				i++;
			}
			bFoundMem=TRUE;
		}
		else if (ChunkId[0]=='S' && ChunkId[1]=='t' && ChunkId[2]=='k' && ChunkId[3]=='s')
		{
			while (stackDepth(&m_stack))
			{
				stackPop(&m_stack);
			}
			while (stackDepth(&m_callStack))
			{
				stackPop(&m_callStack);
			}
			while (stream->pos<ChunkEnd)
			{
				ZCallStack* CS=(ZCallStack*)stackPush(&m_callStack);
				int StackDepth;
				CS->returnAddr=readByte(stream)<<16;
				CS->returnAddr+=readByte(stream)<<8;
				CS->returnAddr+=readByte(stream);
				CS->numLocals=readByte(stream)&0x0F;
				CS->returnStore=readByte(stream);
				CS->setArguments=readByte(stream);
				CS->depth=stackDepth(&m_stack);
				StackDepth=readByte(stream)<<8;
				StackDepth+=readByte(stream);
				for (int s=0; s<CS->numLocals; s++)
				{
					uint8_t MSB=readByte(stream);
					CS->locals[s]=makeS16(MSB, readByte(stream));
				}
				while (StackDepth--)
				{
					uint8_t MSB=readByte(stream);
					*(s16*)stackPush(&m_stack)=makeS16(MSB, readByte(stream));
				}
			}
			bFoundStacks=TRUE;
		}
		while (stream->pos<NextChunk)
		{
			(void)readByte(stream);
		}
	}
	return (bFoundHeader && bFoundMem && bFoundStacks);
}

void saveInstruction()
{
	ScreenPrint("Select a file name (filename.qzl):");
	strcpy(m_input, "/spiflash/");
	ScreenReadInput(m_input + sizeof("/spiflash/") - 1, sizeof(m_input) - sizeof("/spiflash/"));
	// Prime with chunk sizes
	saveResetStream(&m_savestream);
	saveState(&m_savestream);
	// Actually write to file
	saveResetStream(&m_savestream);
	m_savestream.f=fopen(m_input,"wb");
	if (m_savestream.f)
	{
		saveState(&m_savestream);
		fclose(m_savestream.f);
	}
	if (m_version>=4)
		setVariable(m_ins.store, m_savestream.f?1:0);
	else
		doBranch(m_savestream.f!=NULL, m_ins.branch);
}

void restoreInstruction()
{
	ScreenPrint("Select a file name (filename.qzl):");
	strcpy(m_input, "/spiflash/");
	ScreenReadInput(m_input + sizeof("/spiflash/") - 1, sizeof(m_input) - sizeof("/spiflash/"));
	saveResetStream(&m_savestream);
	m_savestream.f = fopen(m_input, "rb");
	if (m_savestream.f)
	{
		if (restoreState(&m_savestream))
		{
			fclose(m_savestream.f);
			if (m_version==4)
			{
				m_ins.store=readStoreInstruction(zeroOpStoreInstructionsV4,ARRAY_SIZEOF(zeroOpStoreInstructionsV4),m_ins.op);
				setVariable(m_ins.store, 2);
			}
			else if (m_version>=5)
			{
				m_ins.store=readStoreInstruction(extOpStoreInstructions,ARRAY_SIZEOF(extOpStoreInstructions),m_ins.op);
				setVariable(m_ins.store, 2);
			}
			else
			{
				m_ins.branch=readBranchInstruction(zeroOpBranchInstructions,ARRAY_SIZEOF(zeroOpBranchInstructions),5);
				doBranch(TRUE, m_ins.branch);
			}
			return;
		}
		fclose(m_savestream.f);
	}
	if (m_version>=4)
		setVariable(m_ins.store, 0);
	else
		doBranch(FALSE, m_ins.branch);
}

void process0OPInstruction()
{
	switch (m_ins.op)
	{
		case 0: //rtrue
			returnRoutine(1);
			break;
		case 1: //rfalse
			returnRoutine(0);
			break;
		case 2: //print
			m_pc=printText(m_pc);
			break;
		case 3: //print_ret
			m_pc=printText(m_pc);
			printToStream('\n');
			returnRoutine(1);
			break;
		case 4: //nop
			break;
		case 5: //save
			saveInstruction();
			break;
		case 6: //restore
			restoreInstruction();
			break;
		case 7: //restart
			haltInstruction();
			break;
		case 8: //ret_popped
			returnRoutine(*(s16*)stackPop(&m_stack));
			break;
		case 9:
			if (m_version<5)
				stackPop(&m_stack); // pop
			else
				setVariable(m_ins.store, stackDepth(&m_callStack)); // catch
			break;
		case 0xA: //quit
			exit(0);
			break;
		case 0xB: //new_line
			printToStream('\n');
			break;
		case 0xC: //show_status
			updateStatus();
			break;
		case 0xD: //verify
			doBranch(TRUE, m_ins.branch);
			break;
		case 0xE: //extended
			illegalInstruction();
			break;
		case 0xF: //piracy
			doBranch(TRUE, m_ins.branch);
			break;
	}
}

void process1OPInstruction()
{
	switch (m_ins.op)
	{
		case 0: //jz
			doBranch(m_ins.operands[0].value==0, m_ins.branch);
			break;
		case 1: //get_sibling
			{
				if (m_ins.operands[0].value==0)
				{
					setVariable(m_ins.store, 0);
					doBranch(FALSE, m_ins.branch);
					break;
				}
				ZObject child=getObject(m_ins.operands[0].value);
				int siblingId=zobjectGetSibling(child);
				setVariable(m_ins.store, siblingId);
				doBranch(siblingId!=0, m_ins.branch);
#if FROTZ_WATCHING
				printf("@get_sibling ");
				printText(child.propTable+1);
				printf("\n");
#endif
				break;
			}
		case 2: //get_child
			{
				if (m_ins.operands[0].value==0)
				{
					setVariable(m_ins.store, 0);
					doBranch(FALSE, m_ins.branch);
					break;
				}
				ZObject child=getObject(m_ins.operands[0].value);
				int childId=zobjectGetChild(child);
				setVariable(m_ins.store, childId);
				doBranch(childId!=0, m_ins.branch);
#if FROTZ_WATCHING
				printf("@get_child ");
				printText(child.propTable+1);
				printf("\n");
#endif
				break;
			}
		case 3: //get_parent_object
			{
				if (m_ins.operands[0].value==0)
				{
					setVariable(m_ins.store, 0);
					break;
				}
				ZObject child=getObject(m_ins.operands[0].value);
				setVariable(m_ins.store, zobjectGetParent(child));
#if FROTZ_WATCHING
				printf("@get_parent ");
				printText(child.propTable+1);
				printf("\n");
#endif
				break;
			}
		case 4: //get_prop_len
			{
				int propAddress=(m_ins.operands[0].value&0xFFFF);
				int size=0;
				if (propAddress>0)
				{
					int sizeId=ReadMemory(propAddress-1)&0xFF;
					size=(sizeId>>5)+1;
					if (m_version>3)
					{
						if (sizeId&0x80)
						{
							size=sizeId&63;
							if (size==0)
								size=64;
						}
						else
						{
							size=(sizeId&0x40)?2:1;
						}
					}
				}
				setVariable(m_ins.store, size);
				break;
			}
		case 5: //inc
			{
				int value=readVariable(m_ins.operands[0].value);
				setVariable(m_ins.operands[0].value, value+1);
				break;
			}
		case 6: //dec
			{
				int value=readVariable(m_ins.operands[0].value);
				setVariable(m_ins.operands[0].value, value-1);
				break;
			}	
		case 7: //print_addr
			printText(m_ins.operands[0].value&0xFFFF);
			break;
		case 8: //call_1s
			callRoutine(m_packedMultiplier*(m_ins.operands[0].value&0xFFFF), m_ins.store, TRUE);
			break;
		case 9: //remove_obj
			{
				if (m_ins.operands[0].value==0)
					break;
				removeObject(m_ins.operands[0].value);
#if FROTZ_WATCHING
				printf("@remove_obj ");
				printText(getObject(m_ins.operands[0].value).propTable+1);
				printf("\n");
#endif
				break;
			}
		case 0xA: //print_obj
			{
				if (m_ins.operands[0].value==0)
					break;
				ZObject obj=getObject(m_ins.operands[0].value);
				printText(obj.propTable+1);
				break;
			}
		case 0xB: //ret
			returnRoutine(m_ins.operands[0].value);
			break;
		case 0xC: //jump
			m_pc+=m_ins.operands[0].value-2;
			break;
		case 0xD: //print_paddr
			printText(m_packedMultiplier*(m_ins.operands[0].value&0xFFFF));
			break;
		case 0xE: //load
			setVariable(m_ins.store, readVariableIndirect(m_ins.operands[0].value));
			break;
		case 0xF:
			if (m_version<=4)
				setVariable(m_ins.store, ~m_ins.operands[0].value); //not
			else
				callRoutine(m_packedMultiplier*(m_ins.operands[0].value&0xFFFF), m_ins.store, TRUE); //call_1n
			break;
	}
}

void process2OPInstruction()
{
	switch (m_ins.op)
	{
		case 0:
			illegalInstruction();
			break;
		case 1: //je
			{
				int takeBranch=FALSE;
				int test=m_ins.operands[0].value;
				int i;
				for (i=1; i<m_ins.numOps; i++)
				{
					if (test==m_ins.operands[i].value)
					{
						takeBranch=TRUE;
						break;
					}
				}
				doBranch(takeBranch, m_ins.branch);
				break;
			}
		case 2: //jl
			doBranch(m_ins.operands[0].value<m_ins.operands[1].value, m_ins.branch);
			break;
		case 3: //jg
			doBranch(m_ins.operands[0].value>m_ins.operands[1].value, m_ins.branch);
			break;
		case 4: //dec_chk
			{
				short value=readVariable(m_ins.operands[0].value);
				value--;
				setVariable(m_ins.operands[0].value, value);
				doBranch(value<m_ins.operands[1].value, m_ins.branch);
				break;
			}	
		case 5: //inc_chk
			{
				short value=readVariable(m_ins.operands[0].value);
				value++;
				setVariable(m_ins.operands[0].value, value);
				doBranch(value>m_ins.operands[1].value, m_ins.branch);
				break;
			}
		case 6: //jin
			{
				if (m_ins.operands[0].value==0 || m_ins.operands[1].value==0)
				{
					// strict.z5 wants us to return nothing is in nothing
					doBranch(m_ins.operands[0].value==m_ins.operands[1].value, m_ins.branch);
					break;
				}
				ZObject child=getObject(m_ins.operands[0].value);
				doBranch(zobjectGetParent(child)==m_ins.operands[1].value, m_ins.branch);
#if FROTZ_WATCHING
				printf("@jin ");
				printText(child.propTable+1);
				printf(" ");
				printText(getObject(m_ins.operands[1].value).propTable+1);
				printf("\n");
#endif
				break;
			}
		case 7: //test
			{
				int flags=m_ins.operands[1].value;
				doBranch((m_ins.operands[0].value&flags)==flags, m_ins.branch);
				break;
			}
		case 8: //or
			setVariable(m_ins.store, m_ins.operands[0].value|m_ins.operands[1].value);
			break;
		case 9: //and
			setVariable(m_ins.store, m_ins.operands[0].value&m_ins.operands[1].value);
			break;
		case 0xA: //test_attr
			{
				if (m_ins.operands[0].value==0)
				{
					doBranch(FALSE, m_ins.branch);
					break;
				}
				ZObject obj=getObject(m_ins.operands[0].value);
				int attr=m_ins.operands[1].value;
				int offset=attr/8;
				int bit=0x80>>(attr%8);
				int test=FALSE;
				if (attr<32 || (m_version>3 && attr<48))
					test=((ReadMemory(obj.addr+offset)&bit)==bit);
				doBranch(test, m_ins.branch);
#if FROTZ_WATCHING
				printf("@test_attr ");
				printText(obj.propTable+1);
				printf(" %d\n", attr);
#endif
				break;
			}
		case 0xB: //set_attr
			{
				if (m_ins.operands[0].value==0)
					break;
				ZObject obj=getObject(m_ins.operands[0].value);
				int attr=m_ins.operands[1].value;
				int offset=attr/8;
				int bit=0x80>>(attr%8);
				if (attr<32 || (m_version>3 && attr<48))
					SetMemory(obj.addr+offset, ReadMemory(obj.addr+offset)|bit);
#if FROTZ_WATCHING
				printf("@set_attr ");
				printText(obj.propTable+1);
				printf(" %d\n", attr);
#endif
				break;
			}
		case 0xC: //clear_attr
			{
				if (m_ins.operands[0].value==0)
					break;
				ZObject obj=getObject(m_ins.operands[0].value);
				int attr=m_ins.operands[1].value;
				int offset=attr/8;
				int bit=0x80>>(attr%8);
				if (attr<32 || (m_version>3 && attr<48))
					SetMemory(obj.addr+offset, ReadMemory(obj.addr+offset)&~bit);
#if FROTZ_WATCHING
				printf("@clear_attr ");
				printText(obj.propTable+1);
				printf(" %d\n", attr);
#endif
				break;
			}
		case 0xD: //store
			setVariableIndirect(m_ins.operands[0].value, m_ins.operands[1].value);
			break;
		case 0xE: //insert_obj
			{
				if (m_ins.operands[0].value==0 || m_ins.operands[1].value==0)
				{
					break;
				}
				removeObject(m_ins.operands[0].value);
				addChild(m_ins.operands[1].value, m_ins.operands[0].value);
#if FROTZ_WATCHING
				printf("@remove_obj ");
				printText(getObject(m_ins.operands[0].value).propTable+1);
				printf(" ");
				printText(getObject(m_ins.operands[1].value).propTable+1);
				printf("\n");
#endif
				break;
			}
		case 0xF: //loadw
			{
				int address=((m_ins.operands[0].value&0xFFFF)+2*(m_ins.operands[1].value));
				setVariable(m_ins.store, makeS16(ReadMemory(address)&0xFF, ReadMemory(address+1)&0xFF));
				break;
			}
		case 0x10: //loadb
			{
				int address=((m_ins.operands[0].value&0xFFFF)+(m_ins.operands[1].value));
				setVariable(m_ins.store, ReadMemory(address)&0xFF);
				break;
			}
		case 0x11: //get_prop
			{
				if (m_ins.operands[0].value==0)
				{
					setVariable(m_ins.store,0);
					break;
				}
				ZObject obj=getObject(m_ins.operands[0].value);
				ZProperty prop=getProperty(obj, m_ins.operands[1].value);
				if (prop.size==1)
				{
					setVariable(m_ins.store, ReadMemory(prop.addr)&0xFF);
				}
				else if (prop.size==2)
				{
					setVariable(m_ins.store, makeS16(ReadMemory(prop.addr)&0xFF, ReadMemory(prop.addr+1)&0xFF));
				}
				else
				{
					illegalInstruction();
				}
				break;
			}
		case 0x12: //get_prop_addr
			{
				if (m_ins.operands[0].value==0)
				{
					setVariable(m_ins.store,0);
					break;
				}
				ZObject obj=getObject(m_ins.operands[0].value);
				ZProperty prop=getProperty(obj, m_ins.operands[1].value);
				if (prop.bDefault)
					setVariable(m_ins.store, 0);
				else
					setVariable(m_ins.store, prop.addr);
				break;
			}
		case 0x13: //get_next_prop
			{
				if (m_ins.operands[0].value==0)
				{
					setVariable(m_ins.store,0);
					break;
				}
				ZObject obj=getObject(m_ins.operands[0].value);
				int propMask=(m_version<=3)?31:63;
				if (m_ins.operands[1].value==0)
				{
					int address=obj.propTable;
					int textLen=ReadMemory(address++)&0xFF;
					address+=textLen*2;
					int nextSizeId=ReadMemory(address)&0xFF;
					setVariable(m_ins.store, nextSizeId&propMask);
				}
				else
				{
					ZProperty prop=getProperty(obj, m_ins.operands[1].value);
					if (prop.bDefault)
					{
						illegalInstruction();
					}
					else
					{
						int nextSizeId=ReadMemory(prop.addr+prop.size)&0xFF;
						setVariable(m_ins.store, nextSizeId&propMask);
					}
				}
				break;
			}
		case 0x14: //add
			setVariable(m_ins.store, m_ins.operands[0].value+m_ins.operands[1].value);
			break;
		case 0x15: //sub
			setVariable(m_ins.store, m_ins.operands[0].value-m_ins.operands[1].value);
			break;
		case 0x16: //mul
			setVariable(m_ins.store, m_ins.operands[0].value*m_ins.operands[1].value);
			break;
		case 0x17: //div
			setVariable(m_ins.store, m_ins.operands[0].value/m_ins.operands[1].value);
			break;
		case 0x18: //mod
			setVariable(m_ins.store, m_ins.operands[0].value%m_ins.operands[1].value);
			break;
		case 0x19: //call_2s
			callRoutine(m_packedMultiplier*(m_ins.operands[0].value&0xFFFF), m_ins.store, TRUE);
			break;
		case 0x1A: //call_2n
			callRoutine(m_packedMultiplier*(m_ins.operands[0].value&0xFFFF), m_ins.store, TRUE);
			break;
		case 0x1B: //set_colour
			illegalInstruction();
			break;
		case 0x1C: //throw
			while (m_ins.operands[1].value<stackDepth(&m_callStack))
			{
				stackPop(&m_callStack);
			}
			returnRoutine(m_ins.operands[0].value);
			break;
		case 0x1D:
		case 0x1E:
		case 0x1F:
			illegalInstruction();
			break;
	}
}

void processVARInstruction()
{
	switch (m_ins.op)
	{
		case 0: // call
			callRoutine(m_packedMultiplier*(m_ins.operands[0].value&0xFFFF), m_ins.store, TRUE);
			break;
		case 1: //storew
			{
				int address=((m_ins.operands[0].value&0xFFFF)+2*m_ins.operands[1].value);
				int value=m_ins.operands[2].value;
				SetMemory(address, (byte)((value>>8)&0xFF));
				SetMemory(address+1, (byte)(value&0xFF));
				break;
			}
		case 2: //storeb
			{
				int address=((m_ins.operands[0].value&0xFFFF)+m_ins.operands[1].value);
				int value=m_ins.operands[2].value;
				SetMemory(address, (byte)(value&0xFF));
				break;
			}
		case 3: //put_prop
			{
				if (m_ins.operands[0].value==0)
					break;
				ZObject obj=getObject(m_ins.operands[0].value);
				ZProperty prop=getProperty(obj, m_ins.operands[1].value);
				if (!prop.bDefault)
				{
					if (prop.size==1)
					{
						SetMemory(prop.addr, (byte)(m_ins.operands[2].value&0xFF));
					}
					else if (prop.size==2)
					{
						SetMemory(prop.addr+0, (byte)((m_ins.operands[2].value>>8)&0xFF));
						SetMemory(prop.addr+1, (byte)(m_ins.operands[2].value&0xFF));
					}
				}
				else
				{
					illegalInstruction();
				}
				break;
			}
		case 4: //sread
			{
				int bufferAddr=(m_ins.operands[0].value&0xFFFF);
				int parseAddr=(m_ins.operands[1].value&0xFFFF);
				int maxLength=ReadMemory(bufferAddr++)&0xFF;
				int realInLen=0;
				int inLen;
				int stringLengthAddr=0;
				int i;
				if (m_ins.numOps>2) // timed input not supported
					illegalInstruction();
				updateStatus();
				ioReadInput(m_input, sizeof(m_input), FALSE);
				inLen=strlen(m_input);
				if (m_version>=5)
					stringLengthAddr=bufferAddr++;
				for (i=0; i<inLen && i<maxLength; i++)
				{
					if (m_input[i]!='\r' && m_input[i]!='\n')
					{
						m_input[realInLen++]=tolower((int)m_input[i]);
						SetMemory(bufferAddr++,(byte)m_input[i]);
					}
				}
				m_input[realInLen]='\0';
				if (m_version<5)
					SetMemory(bufferAddr++,0);
				else
					SetMemory(stringLengthAddr, realInLen);
				if (m_version<5 || parseAddr!=0)
					lexicalAnalysis(m_input, parseAddr, m_dictionaryTable, FALSE);
				if (m_version>=5)
					setVariable(m_ins.store, '\r');
				break;
			}
		case 5: //print_char
			printToStream((char)m_ins.operands[0].value);
			break;
		case 6: //print_num
			{
				char numString[16];
				char *numPtr=numString;
				sprintf(numString,"%d", m_ins.operands[0].value);
				while (*numPtr)
				{
					printToStream(*(numPtr++));
				}
				break;
			}
		case 7: //random
			{
				int maxValue=m_ins.operands[0].value;
				int ret=0;
				if (maxValue>0)
				{
					ret=(ioRandom()%maxValue)+1;
				}
				setVariable(m_ins.store, ret);
				break;
			}
		case 8: //push
			setVariable(0, m_ins.operands[0].value);
			break;
		case 9: //pull
			setVariableIndirect(m_ins.operands[0].value, readVariable(0));
			break;
		case 0xA: //split_window
			if (m_ins.operands[0].value==0)
			{
				ioUnsplitWindow();
			}
			else
			{
				ioSplitWindow(m_ins.operands[0].value);
				if (m_version==3)
					ioEraseWindow(1);
			}
			break;
		case 0xB: //set_window
			ioSetWindow(m_ins.operands[0].value, m_ins.operands[0].value==1);
			break;
		case 0xC: //call_vs2
			callRoutine(m_packedMultiplier*(m_ins.operands[0].value&0xFFFF), m_ins.store, TRUE);
			break;
		case 0xD: //erase_window
			if (m_ins.operands[0].value==-1)
			{
				ioUnsplitWindow();
				ioEraseWindow(0);
			}
			else if (m_ins.operands[0].value==-2)
			{
				ioEraseWindow(0);
				ioEraseWindow(1);
			}
			else
			{
				ioEraseWindow(m_ins.operands[0].value);
			}
			break;
		case 0xE: //erase_line
			if (m_ins.operands[0].value==1)
			{
				ioEraseToEndOfLine();
			}
			break;
		case 0xF: //set_cursor
			ioSetCursor(m_ins.operands[1].value-1, m_ins.operands[0].value-1);
			break;
		case 0x10: //get_cursor
			{
				int array=m_ins.operands[0].value&0xFFFF;
				int x,y;
				ioGetCursor(&x,&y);
				x++; // not zero based index in Z-machine
				y++;
				SetMemory(array+0,(y>>8)&0xFF);
				SetMemory(array+1,y&0xFF);
				SetMemory(array+2,(x>>8)&0xFF);
				SetMemory(array+3,x&0xFF);
			}
			break;
		case 0x11: //set_text_style
			if (m_ins.operands[0].value==0)
				ioClearStyleBits();
			else
				ioSetStyleBits(m_ins.operands[0].value);
			break;
		case 0x12: //buffer_mode
			// Don't care, we do word wrap differently
			break;
		case 0x13: //output_stream
			if (abs(m_ins.operands[0].value)==3)
			{
				if (m_ins.operands[0].value>0 && m_ins.numOps>=2)
				{
					m_memStreamPtr=(m_ins.operands[0].value>0 && m_ins.numOps>=2)?m_ins.operands[1].value&0xFFFF:0;
					SetMemory(m_memStreamPtr+0,0);
					SetMemory(m_memStreamPtr+1,0);
				}
				else
				{
					m_memStreamPtr=0;
				}
			}
			else if (abs(m_ins.operands[0].value)==1)
			{
				m_outputStream=(m_ins.operands[0].value>0);
			}
			break;
		case 0x14: //input_stream
			if (m_ins.operands[0].value!=0)
				haltInstruction(); // We don't support transcription
			break;
		case 0x15: //sound_effect
			// Don't care, we have no sound HW
			break;
		case 0x16: //read_char
			{
				int oldWindow = ioSetWindow(2, FALSE);
				ioSetCursor(NUM_COLS-3, NUM_ROWS-1); // Will clear line so put cursor at bottom right
				ioReadInput(m_input, sizeof(m_input), TRUE);
				ioSetWindow(oldWindow, FALSE);
				setVariable(m_ins.store, m_input[0]?m_input[0]:'\r');
				break;
			}
		case 0x17: //scan_table
			{
				int x=(m_ins.operands[0].value&0xFFFF);
				int table=(m_ins.operands[1].value&0xFFFF);
				int len=(m_ins.operands[2].value&0xFFFF);
				int form=(m_ins.numOps>3?m_ins.operands[3].value&0xFFFF:0x82);
				int found=0;
				int i;
				for (i=0;i<len;i++)
				{
					if (((form&0x80)!=0 && x==makeU16(ReadMemory(table)&0xFF, ReadMemory(table+1)&0xFF)) ||
						((form&0x80)==0 && x==(ReadMemory(table)&0xFF)))
					{
						found=table;
						break;
					}
					table+=(form&0x7F);
				}
				setVariable(m_ins.store, found);
				doBranch(found!=0, m_ins.branch);
				break;
			}
		case 0x18: //not
			setVariable(m_ins.store, ~m_ins.operands[0].value);
			break;
		case 0x19: //call_vn
			callRoutine(m_packedMultiplier*(m_ins.operands[0].value&0xFFFF), m_ins.store, TRUE);
			break;
		case 0x1A: //call_vn2
			callRoutine(m_packedMultiplier*(m_ins.operands[0].value&0xFFFF), m_ins.store, TRUE);
			break;
		case 0x1B: //tokenise
			{
				// Nb. Never seen in game so consider untested
				int bufferAddr=(m_ins.operands[0].value&0xFFFF);
				int parseAddr=(m_ins.operands[1].value&0xFFFF);
				int dictionaryTable=(m_ins.numOps>2 && m_ins.operands[2].value!=0)?m_ins.operands[2].value&0xFFFF:m_dictionaryTable;
				int ignoreUnknown=(m_ins.numOps>3)?m_ins.operands[3].value:FALSE;
				int length=ReadMemory(bufferAddr+1);
				char *input=m_input;
				bufferAddr+=2;
				while (length--)
				{
					*input=ReadMemory(bufferAddr++);
					if (*input=='\0')
						break;
					input++;
				}
				*input='\0';
				lexicalAnalysis(m_input, parseAddr, dictionaryTable, ignoreUnknown);
				break;
			}
		case 0x1C: //encode_text
			{
				// Nb. Never seen in game so consider untested
				char token[10]={0};
				int text=(m_ins.operands[0].value&0xFFFF)+m_ins.operands[2].value;
				int length=m_ins.operands[1].value;
				int codedOutput=(m_ins.operands[3].value&0xFFFF);
				int i;
				ZDictEntry zde;
				for (i=0;i<9 && i<length;i++)
				{
					token[i]=ReadMemory(text+i);
					if (token[i]=='\0')
						break;
				}
				zde = encodeToken(token);
				for (i=0;i<6;i++)
					SetMemory(codedOutput+i, zde.coded[i]);
				break;
			}
		case 0x1D: //copy_table
			{
				int first=(m_ins.operands[0].value&0xFFFF);
				int second=(m_ins.operands[1].value&0xFFFF);
				int size=m_ins.operands[2].value;
				if (second==0)
				{
					while (size--)
						SetMemory(first++,0);
				}
				else if (first>second || size<0)
				{
					size=abs(size);
					while (size--)
						SetMemory(second++, ReadMemory(first++));
				}
				else
				{
					size=abs(size);
					first+=size;
					second+=size;
					while (size--)
						SetMemory(--second, ReadMemory(--first));
				}
				break;
			}
		case 0x1E: //print_table
			{
				int text=(m_ins.operands[0].value&0xFFFF);
				int height=m_ins.numOps>2?(m_ins.operands[2].value&0xFFFF):1;
				int skip=m_ins.numOps>3?(m_ins.operands[3].value&0xFFFF):0;
				int x,y;
				int curX,curY;
				ioGetCursor(&curX,&curY);
				for (y=0; y<height; y++)
				{
					ioSetCursor(curX,curY++);
					for (x=0; x<(m_ins.operands[1].value&0xFFFF); x++)
						printToStream(ReadMemory(text++));
					text+=skip;
				}
				break;
			}
		case 0x1F: //check_arg_count
			{
				char setArguments=((ZCallStack*)stackPeek(&m_callStack))->setArguments;
				int testArguments=m_ins.operands[0].value-1;
				int test=TRUE;
				if (testArguments>=0)
					test=(setArguments&(1<<testArguments));
				doBranch(test, m_ins.branch);
				break;
			}
	}
}

void processEXTInstruction()
{
	switch (m_ins.op)
	{
		case 0: // save
			if (m_ins.numOps>0)
			{
				setVariable(m_ins.store,0);
				break;
			}
			saveInstruction();
			break;
		case 1: // restore
			if (m_ins.numOps>0)
			{
				setVariable(m_ins.store,0);
				break;
			}
			restoreInstruction();
			break;
		case 2: // log_shift
			if (m_ins.operands[1].value>=0)
				setVariable(m_ins.store,(m_ins.operands[0].value&0xFFFF)<<(m_ins.operands[1].value));
			else
				setVariable(m_ins.store,(m_ins.operands[0].value&0xFFFF)>>abs(m_ins.operands[1].value));
			break;
		case 3: // art_shift
			if (m_ins.operands[1].value>=0)
				setVariable(m_ins.store,m_ins.operands[0].value<<(m_ins.operands[1].value));
			else
				setVariable(m_ins.store,m_ins.operands[0].value>>abs(m_ins.operands[1].value));
			break;
			break;
		case 4: // set_font
			// Don't care about fonts
			break;
		case 9: // save_undo
			setVariable(m_ins.store,-1);
			break;
		case 0xA: // restore_undo
			setVariable(m_ins.store,0);
			break;
		case 0xB: // print_unicode
			haltInstruction();
			break;
		case 0xC: // check_unicode
			haltInstruction();
			break;
		case 0xD: // set_true_colour
			haltInstruction();
			break;
		default:
			illegalInstruction();
			break;
	}
}

void executeInstruction()
{
	m_ins.numOps=0;
	//printf("%04x\n", m_pc);
	int opcode=readBytePC();
	if (m_version>=5 && opcode==0xBE)
	{
		readExtendedForm();
	}
	else if ((opcode&0xC0)==0xC0)
	{
		readVariableForm(opcode);
	}
	else if ((opcode&0xC0)==0x80)
	{
		readShortForm(opcode);
	}
	else
	{
		readLongForm(opcode);
	}
	switch (m_ins.form)
	{
		case Form0OP:
			if (m_version<4)
			{
				m_ins.store=readStoreInstruction(zeroOpStoreInstructions,ARRAY_SIZEOF(zeroOpStoreInstructions),m_ins.op);
				m_ins.branch=readBranchInstruction(zeroOpBranchInstructions,ARRAY_SIZEOF(zeroOpBranchInstructions),m_ins.op);
			}
			else
			{
				m_ins.store=readStoreInstruction(zeroOpStoreInstructionsV4,ARRAY_SIZEOF(zeroOpStoreInstructionsV4),m_ins.op);
				m_ins.branch=readBranchInstruction(zeroOpBranchInstructionsV4,ARRAY_SIZEOF(zeroOpBranchInstructionsV4),m_ins.op);
			}
			//dumpCurrentInstruction();
			process0OPInstruction();
			break;
		case Form1OP:
			if (m_version<5)
				m_ins.store=readStoreInstruction(oneOpStoreInstructions,ARRAY_SIZEOF(oneOpStoreInstructions),m_ins.op);
			else
				m_ins.store=readStoreInstruction(oneOpStoreInstructionsV5,ARRAY_SIZEOF(oneOpStoreInstructionsV5),m_ins.op);
			m_ins.branch=readBranchInstruction(oneOpBranchInstructions,ARRAY_SIZEOF(oneOpBranchInstructions),m_ins.op);
			//dumpCurrentInstruction();
			process1OPInstruction();
			break;
		case Form2OP:
			m_ins.store=readStoreInstruction(twoOpStoreInstructions,ARRAY_SIZEOF(twoOpStoreInstructions),m_ins.op);
			m_ins.branch=readBranchInstruction(twoOpBranchInstructions,ARRAY_SIZEOF(twoOpBranchInstructions),m_ins.op);
			//dumpCurrentInstruction();
			process2OPInstruction();
			break;
		case FormVAR:
			if (m_version<=4)
				m_ins.store=readStoreInstruction(varOpStoreInstructions,ARRAY_SIZEOF(varOpStoreInstructions),m_ins.op);
			else
				m_ins.store=readStoreInstruction(varOpStoreInstructionsV5,ARRAY_SIZEOF(varOpStoreInstructionsV5),m_ins.op);
			m_ins.branch=readBranchInstruction(varOpBranchInstructions,ARRAY_SIZEOF(varOpBranchInstructions),m_ins.op);
			//dumpCurrentInstruction();
			processVARInstruction();
			break;
		case FormEXT:
			m_ins.store=readStoreInstruction(extOpStoreInstructions,ARRAY_SIZEOF(extOpStoreInstructions),m_ins.op);
			m_ins.branch=readBranchInstruction(extOpBranchInstructions,ARRAY_SIZEOF(extOpBranchInstructions),m_ins.op);
			//dumpCurrentInstruction();
			processEXTInstruction();
			break;
	}
}

void allocateDynamic()
{
	// Allocate memory in small chunks to help if memory is fragmented
	// (To help when running big games on ESP32)
	int dynamicLeft=m_endOfDynamic;
	int numChunks=0;
	int i;
	memset(dynamicChunks,0,sizeof(dynamicChunks));
	while (dynamicLeft>0)
	{
		int chunkSize=(dynamicLeft>DYNAMIC_CHUNK)?DYNAMIC_CHUNK:dynamicLeft;
		dynamicChunks[numChunks++]=malloc(chunkSize);
		dynamicLeft-=chunkSize;
	}
	for (i=0; i<m_endOfDynamic; i++)
	{
		DYNAMIC(i) = memory[i];
	}
}

void zopsMain(const char* GameData)
{
	byte Flags = 0;

	memory = GameData;
	m_endOfDynamic = makeU16(memory[0xE] & 0xFF, memory[0xF] & 0xFF);
	printf("%d bytes dynamic\n", m_endOfDynamic);
	ASSERT(m_endOfDynamic <= 64 * 1024);
	allocateDynamic();

	m_version = ReadMemory(0);
	ASSERT((m_version >= 3 && m_version <= 5) || m_version == 8);
	m_memStreamPtr = 0;
	m_outputStream = TRUE;
	m_packedMultiplier = (m_version == 8) ? 8 : (m_version >= 4 ? 4 : 2);
	m_globalVariables = makeU16(ReadMemory(0xC) & 0xFF, ReadMemory(0xD) & 0xFF);
	m_abbrevTable = makeU16(ReadMemory(0x18) & 0xFF, ReadMemory(0x19) & 0xFF);
	m_objectTable = makeU16(ReadMemory(0xA) & 0xFF, ReadMemory(0xB) & 0xFF);
	m_dictionaryTable = makeU16(ReadMemory(0x8) & 0xFF, ReadMemory(0x9) & 0xFF);
	m_pc = makeU16(ReadMemory(6) & 0xFF, ReadMemory(7) & 0xFF);
	Flags = ReadMemory(1);
	if (m_version == 3)
	{
		//Flags|=(1<<4); // status line not available
		Flags |= (1 << 5);	// screen splitting available
		Flags &= ~(1 << 6); // variable pitch font
	}
	else
	{
		Flags = (1 << 2);  // bold
		Flags |= (1 << 3); // italics
	}
	SetMemory(1, Flags);
	Flags = makeU16(ReadMemory(0x10) & 0xFF, ReadMemory(0x11) & 0xFF);
	Flags &= ~(1 << 0); // not transcripting
	Flags |= (1 << 1);	// fixed font
	if (m_version >= 5)
	{
		Flags &= 3; // No fancy stuff supported
	}
	SetMemory(0x10, Flags >> 8);
	SetMemory(0x11, Flags & 0xFF);
	if (m_version >= 4)
	{
		SetMemory(0x20, 23); // Screen height
		SetMemory(0x21, 80); // Screen width
	}
	if (m_version >= 5)
	{
		int customAlphabet = makeU16(ReadMemory(0x34) & 0xFF, ReadMemory(0x35) & 0xFF);
		if (customAlphabet != 0)
		{
			int i;
			for (i = 0; i < sizeof(alphabetLookup[2]); i++)
			{
				alphabetLookup[2][i] = ReadMemory(customAlphabet + i);
			}
		}
		SetMemory(0x22, 0);	 // Screen width (in units)
		SetMemory(0x23, 80); // Screen width (in units)
		SetMemory(0x24, 0);	 // Screen height (in units)
		SetMemory(0x25, 23); // Screen height (in units)
		SetMemory(0x26, 1);	 // Font width (in units)
		SetMemory(0x27, 1);	 // Font height (in units)
		SetMemory(0x2C, 2);	 // BG colour
		SetMemory(0x2D, 4);	 // FG colour
	}
	stackInit(&m_stack, m_numberstack, sizeof(m_numberstack[0]), ARRAY_SIZEOF(m_numberstack));
	stackInit(&m_callStack, m_callstackcontents, sizeof(m_callstackcontents[0]), ARRAY_SIZEOF(m_callstackcontents));
	memset(stackPush(&m_callStack), 0, sizeof(ZCallStack)); // Makes save format match Frotz
	ioReset(m_version == 3);
	while (1)
	{
		executeInstruction();
	}
}
