#include <esp_system.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

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

#define MAX_TOKEN_LEN 256

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
	FormVAR
};

static int zeroOpStoreInstructions[]={};
static int oneOpStoreInstructions[]={0x01,0x02,0x03,0x04,0x08,0x0E,0x0F};
static int twoOpStoreInstructions[]={0x08,0x09,0x0F,0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19};
static int varOpStoreInstructions[]={0x00,0x07,0x0C,0x16,0x17,0x18};

static int zeroOpBranchInstructions[]={0x05,0x06,0x0D,0x0F};
static int oneOpBranchInstructions[]={0x00,0x01,0x02};
static int twoOpBranchInstructions[]={0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x0A};
static int varOpBranchInstructions[]={0x17,0x1F};
	
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
	ZOperand operands[4];
} ZInstruction;

typedef struct ZCallStack_s
{
	int returnAddr;
	int returnStore;
	int locals[16];
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
	int coded[4];
	int current;
} ZDictEntry;

typedef char byte;

static ZInstruction m_ins;
static int m_pc;
static int m_globalVariables;
static int m_abbrevTable;
static int m_objectTable;
static int m_dictionaryTable;
static byte *memory;
static int m_numberstack[1024];
static stack m_stack;
static ZCallStack m_callstackcontents[96];
static stack m_callStack;

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
	return memory[m_pc++]&0xFF;
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
		return *(int*)stackPop(&m_stack);
	}
	if (var<16)
	{
		return ((ZCallStack*)stackPeek(&m_callStack))->locals[var-1];
	}
	int off=2*(var-16);
	off+=m_globalVariables;
	return makeS16(memory[off]&0xFF, memory[off+1]&0xFF); 
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
		*(int*)stackPush(&m_stack)=value;
		return;
	}
	if (var<16)
	{
		((ZCallStack*)stackPeek(&m_callStack))->locals[var-1]=value;
		return;
	}
	int off=2*(var-16);
	off+=m_globalVariables;
	memory[off+0]=(byte)((value&0xFF00)>>8);
	memory[off+1]=(byte)((value&0x00FF)>>0); 
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
	ret.addr=m_objectTable+2*31+9*(id-1);
	ret.propTable=makeU16(memory[ret.addr+7]&0xFF, memory[ret.addr+8]&0xFF);
	return ret;
}

ZProperty getProperty(ZObject obj, int id)
{
	ZProperty ret;
	int address=obj.propTable;
	int textLen=memory[address++]&0xFF;
	address+=textLen*2;
	while (memory[address]!=0)
	{
		int sizeId=memory[address++]&0xFF;
		int size=1+(sizeId>>5);
		int propId=sizeId&31;
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
	int i;
	m_ins.op=op;
	if ((opcode&0xF0)>=0xE0)
		m_ins.form=FormVAR;
	else
		m_ins.form=Form2OP;
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
		setVariable(returnStore, 0);
	}
	else
	{
		int numLocals=memory[address++]%0xFF;
		int i;
		ZCallStack cs;
		cs.returnAddr=m_pc;
		cs.returnStore=returnStore;
		for (i=0; i<numLocals; i++)
		{
			cs.locals[i]=makeS16(memory[address]&0xFF, memory[address+1]&0xFF);
			address+=2;
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
	printf("Form:%d Opcode:%d\n", m_ins.form, m_ins.op);
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
		pair1=memory[address++]&0xFF;
		pair2=memory[address++]&0xFF;
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
					printf("%c", (char)longChar);
				}
			}
			else if (!abbrNext)
			{
				if (characters[i]==6 && alphabet==2)
				{
					longNext=2;
				}
				else if (characters[i]>=6)
				{
					characters[i]-=6;
					printf("%c", alphabetLookup[alphabet][characters[i]]);
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
					printf(" ");
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
				int abbrevAddress=makeU16(memory[abbrevTable]&0xFF, memory[abbrevTable+1]&0xFF);
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
	int parentId=memory[child.addr+4]&0xFF;
	if (parentId!=0)
	{
		ZObject parent=getObject(parentId);
		if ((memory[parent.addr+6]&0xFF)==childId)
		{
			memory[parent.addr+6]=memory[child.addr+5]; // parent.child=child.sibling
		}
		else
		{
			int siblingId=memory[parent.addr+6]&0xFF;
			while (siblingId!=0)
			{
				ZObject sibling=getObject(siblingId);
				int nextSiblingId=memory[sibling.addr+5]&0xFF;
				if (nextSiblingId==childId)
				{
					memory[sibling.addr+5]=memory[child.addr+5]; // sibling.sibling=child.sibling
					break;
				}
				siblingId=nextSiblingId;
			}
			if (siblingId==0)
			{
				illegalInstruction();
			}
		}
		memory[child.addr+4]=0;
		memory[child.addr+5]=0;
	}
}

void addChild(int parentId, int childId)
{
	ZObject child=getObject(childId);
	ZObject parent=getObject(parentId);
	memory[child.addr+5]=memory[parent.addr+6]; // child.sibling=parent.child
	memory[child.addr+4]=(byte)parentId; // child.parent=parent
	memory[parent.addr+6]=(byte)childId; // parent.child=child
}

void zDictInit(ZDictEntry *entry)
{
	entry->current=0;
	entry->coded[0]=0;
	entry->coded[1]=0;
	entry->coded[2]=0x80;
	entry->coded[3]=0;
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
	for (t=0; t<6; t++) // pad
	{
		zDictAddCharacter(&ret, 5);
	}
	return ret;
}

int getDictionaryAddress(char* token, int dictionary)
{
	int entryLength = memory[dictionary++]&0xFF;
	int numEntries = makeU16(memory[dictionary+0]&0xFF, memory[dictionary+1]&0xFF);
	ZDictEntry zde = encodeToken(token);
	int i;
	dictionary+=2;
	for (i=0; i<numEntries; i++)
	{
		if (zde.coded[0]==(memory[dictionary+0]&0xFF) && zde.coded[1]==(memory[dictionary+1]&0xFF)
				&& zde.coded[2]==(memory[dictionary+2]&0xFF) && zde.coded[3]==(memory[dictionary+3]&0xFF))
		{
			return dictionary;
		}
		dictionary+=entryLength;
	}
	return 0;
}

int lexicalAnalysis(char* input, int parseBuffer, int maxEntries)
{
	static ZToken tokens[256];
	static char seps[256];
	int numTokens=0;
	int dictionaryAddress=m_dictionaryTable;
	int numSeperators=memory[dictionaryAddress++];
	char *current=input;
	char *end=input+strlen(current);
	int i;
	for (i=0; i<numSeperators; i++)
	{
		seps[i]=(char)memory[dictionaryAddress++];
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
		memory[parseBuffer++]=(byte)((outAddress>>8)&0xFF);
		memory[parseBuffer++]=(byte)((outAddress>>0)&0xFF);
		memory[parseBuffer++]=(byte)strlen(tokens[i].token);
		memory[parseBuffer++]=(byte)(tokens[i].offset+1);
	}

	return MIN(maxEntries, numTokens);
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
			printf("\n");
			returnRoutine(1);
			break;
		case 4: //nop
			break;
		case 5: //save
			doBranch(FALSE, m_ins.branch);
			break;
		case 6: //restore
			doBranch(FALSE, m_ins.branch);
			break;
		case 7: //restart
			haltInstruction();
			break;
		case 8: //ret_popped
			returnRoutine(*(int*)stackPop(&m_stack));
			break;
		case 9: //pop
			stackPop(&m_stack);
			break;
		case 0xA: //quit
			haltInstruction();
			break;
		case 0xB: //new_line
			printf("\n");
			break;
		case 0xC: //show_status
			haltInstruction();
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
				ZObject child=getObject(m_ins.operands[0].value);
				int siblingId=memory[child.addr+5]&0xFF;
				setVariable(m_ins.store, siblingId);
				doBranch(siblingId!=0, m_ins.branch);
				break;
			}
		case 2: //get_child
			{
				ZObject child=getObject(m_ins.operands[0].value);
				int childId=memory[child.addr+6]&0xFF;
				setVariable(m_ins.store, childId);
				doBranch(childId!=0, m_ins.branch);
				break;
			}
		case 3: //get_parent_object
			{
				ZObject child=getObject(m_ins.operands[0].value);
				setVariable(m_ins.store, memory[child.addr+4]&0xFF);
				break;
			}
		case 4: //get_prop_len
			{
				int propAddress=(m_ins.operands[0].value&0xFFFF)-1;
				int sizeId=memory[propAddress]&0xFF;
				int size=(sizeId>>5)+1;
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
			printText(m_ins.operands[0].value);
			break;
		case 8: //call_1s
			illegalInstruction();
			break;
		case 9: //remove_obj
			{
				removeObject(m_ins.operands[0].value);
				break;
			}
		case 0xA: //print_obj
			{
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
			printText(2*(m_ins.operands[0].value&0xFFFF));
			break;
		case 0xE: //load
			setVariable(m_ins.store, readVariableIndirect(m_ins.operands[0].value));
			break;
		case 0xF: //not
			setVariable(m_ins.store, ~m_ins.operands[0].value);
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
				int value=readVariable(m_ins.operands[0].value);
				value--;
				setVariable(m_ins.operands[0].value, value);
				doBranch(value<m_ins.operands[1].value, m_ins.branch);
				break;
			}	
		case 5: //inc_chk
			{
				int value=readVariable(m_ins.operands[0].value);
				value++;
				setVariable(m_ins.operands[0].value, value);
				doBranch(value>m_ins.operands[1].value, m_ins.branch);
				break;
			}
		case 6: //jin
			{
				ZObject child=getObject(m_ins.operands[0].value);
				doBranch((memory[child.addr+4]&0xFF)==m_ins.operands[1].value, m_ins.branch);
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
				ZObject obj=getObject(m_ins.operands[0].value);
				int attr=m_ins.operands[1].value;
				int offset=attr/8;
				int bit=0x80>>(attr%8);
				doBranch((memory[obj.addr+offset]&bit)==bit, m_ins.branch);
				break;
			}
		case 0xB: //set_attr
			{
				ZObject obj=getObject(m_ins.operands[0].value);
				int attr=m_ins.operands[1].value;
				int offset=attr/8;
				int bit=0x80>>(attr%8);
				memory[obj.addr+offset]|=bit;
				break;
			}
		case 0xC: //clear_attr
			{
				ZObject obj=getObject(m_ins.operands[0].value);
				int attr=m_ins.operands[1].value;
				int offset=attr/8;
				int bit=0x80>>(attr%8);
				memory[obj.addr+offset]&=~bit;
				break;
			}
		case 0xD: //store
			setVariableIndirect(m_ins.operands[0].value, m_ins.operands[1].value);
			break;
		case 0xE: //insert_obj
			{
				removeObject(m_ins.operands[0].value);
				addChild(m_ins.operands[1].value, m_ins.operands[0].value);
				break;
			}
		case 0xF: //loadw
			{
				int address=((m_ins.operands[0].value&0xFFFF)+2*(m_ins.operands[1].value&0xFFFF));
				setVariable(m_ins.store, makeS16(memory[address]&0xFF, memory[address+1]&0xFF));
				break;
			}
		case 0x10: //loadb
			{
				int address=((m_ins.operands[0].value&0xFFFF)+(m_ins.operands[1].value&0xFFFF));
				setVariable(m_ins.store, memory[address]&0xFF);
				break;
			}
		case 0x11: //get_prop
			{
				ZObject obj=getObject(m_ins.operands[0].value);
				ZProperty prop=getProperty(obj, m_ins.operands[1].value);
				if (prop.size==1)
				{
					setVariable(m_ins.store, memory[prop.addr]&0xFF);
				}
				else if (prop.size==2)
				{
					setVariable(m_ins.store, makeS16(memory[prop.addr]&0xFF, memory[prop.addr+1]&0xFF));
				}
				else
				{
					illegalInstruction();
				}
				break;
			}
		case 0x12: //get_prop_addr
			{
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
				ZObject obj=getObject(m_ins.operands[0].value);
				if (m_ins.operands[1].value==0)
				{
					int address=obj.propTable;
					int textLen=memory[address++]&0xFF;
					address+=textLen*2;
					int nextSizeId=memory[address]&0xFF;
					setVariable(m_ins.store, nextSizeId&31);
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
						int nextSizeId=memory[prop.addr+prop.size]&0xFF;
						setVariable(m_ins.store, nextSizeId&31);
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
			illegalInstruction();
			break;
		case 0x1A: //call_2n
			illegalInstruction();
			break;
		case 0x1B: //set_colour
			illegalInstruction();
			break;
		case 0x1C: //throw
			illegalInstruction();
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
			callRoutine(2*(m_ins.operands[0].value&0xFFFF), m_ins.store, TRUE);
			break;
		case 1: //storew
			{
				int address=((m_ins.operands[0].value&0xFFFF)+2*(m_ins.operands[1].value&0xFFFF));
				int value=m_ins.operands[2].value;
				memory[address]=(byte)((value>>8)&0xFF);
				memory[address+1]=(byte)(value&0xFF);
				break;
			}
		case 2: //storeb
			{
				int address=((m_ins.operands[0].value&0xFFFF)+(m_ins.operands[1].value&0xFFFF));
				int value=m_ins.operands[2].value;
				memory[address]=(byte)(value&0xFF);
				break;
			}
		case 3: //put_prop
			{
				ZObject obj=getObject(m_ins.operands[0].value);
				ZProperty prop=getProperty(obj, m_ins.operands[1].value);
				if (!prop.bDefault)
				{
					if (prop.size==1)
					{
						memory[prop.addr]=(byte)(m_ins.operands[2].value&0xFF);
					}
					else if (prop.size==2)
					{
						memory[prop.addr+0]=(byte)((m_ins.operands[2].value>>8)&0xFF);
						memory[prop.addr+1]=(byte)(m_ins.operands[2].value&0xFF);
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
				static char input[4096];
				int bufferAddr=m_ins.operands[0].value;
				int parseAddr=m_ins.operands[1].value;
				int maxLength=memory[bufferAddr++]&0xFF;
				int maxParse=memory[parseAddr++]&0xFF;
				int realInLen=0;
				int inLen;
				int i;
				fgets(input, sizeof(input), stdin);
				inLen=strlen(input);
				for (i=0; i<inLen && i<maxLength; i++)
				{
					if (input[i]!='\r' && input[i]!='\n')
					{
						input[realInLen++]=tolower((int)input[i]);
						memory[bufferAddr++]=(byte)input[i];
					}
				}
				input[realInLen]='\0';
				memory[bufferAddr++]=0;
				memory[parseAddr]=(byte)lexicalAnalysis(input, parseAddr+1, maxParse);
				break;
			}
		case 5: //print_char
			printf("%c", (char)m_ins.operands[0].value);
			break;
		case 6: //print_num
			printf("%d", m_ins.operands[0].value);
			break;
		case 7: //random
			{
				int maxValue=m_ins.operands[0].value;
				int ret=0;
				if (maxValue>0)
				{
					ret=(esp_random()%maxValue)+1;
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
			haltInstruction();
			break;
		case 0xB: //set_window
			haltInstruction();
			break;
		case 0xC: //call_vs2
			illegalInstruction();
			break;
		case 0xD: //erase_window
			illegalInstruction();
			break;
		case 0xE: //erase_line
			illegalInstruction();
			break;
		case 0xF: //set_cursor
			illegalInstruction();
			break;
		case 0x10: //get_cursor
			illegalInstruction();
			break;
		case 0x11: //set_text_style
			illegalInstruction();
			break;
		case 0x12: //buffer_mode
			illegalInstruction();
			break;
		case 0x13: //output_stream
			haltInstruction();
			break;
		case 0x14: //input_stream
			haltInstruction();
			break;
		case 0x15: //sound_effect
			haltInstruction();
			break;
		case 0x16: //read_char
			illegalInstruction();
			break;
		case 0x17: //scan_table
			illegalInstruction();
			break;
		case 0x18: //not
			illegalInstruction();
			break;
		case 0x19: //call_vn
			illegalInstruction();
			break;
		case 0x1A: //call_vn2
			illegalInstruction();
			break;
		case 0x1B: //tokenise
			illegalInstruction();
			break;
		case 0x1C: //encode_text
			illegalInstruction();
			break;
		case 0x1D: //copy_table
			illegalInstruction();
			break;
		case 0x1E: //print_table
			illegalInstruction();
			break;
		case 0x1F: //check_arg_count
			illegalInstruction();
			break;
	}
}

void executeInstruction()
{
	m_ins.numOps=0;
	//System.out.println(String.format("%04x", m_pc));
	int opcode=readBytePC();
	if ((opcode&0xC0)==0xC0)
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
			m_ins.store=readStoreInstruction(zeroOpStoreInstructions,ARRAY_SIZEOF(zeroOpStoreInstructions),m_ins.op);
			m_ins.branch=readBranchInstruction(zeroOpBranchInstructions,ARRAY_SIZEOF(zeroOpBranchInstructions),m_ins.op);
			//dumpCurrentInstruction();
			process0OPInstruction();
			break;
		case Form1OP:
			m_ins.store=readStoreInstruction(oneOpStoreInstructions,ARRAY_SIZEOF(oneOpStoreInstructions),m_ins.op);
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
			m_ins.store=readStoreInstruction(varOpStoreInstructions,ARRAY_SIZEOF(varOpStoreInstructions),m_ins.op);
			m_ins.branch=readBranchInstruction(varOpBranchInstructions,ARRAY_SIZEOF(varOpBranchInstructions),m_ins.op);
			//dumpCurrentInstruction();
			processVARInstruction();
			break;
	}
}

void zopsMain(char* GameData)
{
	memory = GameData;
	ASSERT(memory[0] == 3);
	m_globalVariables = makeU16(memory[0xC] & 0xFF, memory[0xD] & 0xFF);
	m_abbrevTable = makeU16(memory[0x18] & 0xFF, memory[0x19] & 0xFF);
	m_objectTable = makeU16(memory[0xA] & 0xFF, memory[0xB] & 0xFF);
	m_dictionaryTable = makeU16(memory[0x8] & 0xFF, memory[0x9] & 0xFF);
	m_pc = makeU16(memory[6] & 0xFF, memory[7] & 0xFF);
	memory[1] |= (1 << 4);	  // status line not available
	memory[1] &= ~(1 << 5);	  // screen splitting available
	memory[1] &= ~(1 << 6);	  // variable pitch font
	memory[0x10] |= (1 << 0); // transcripting
	memory[0x10] |= (1 << 1); // fixed font
	stackInit(&m_stack, m_numberstack, sizeof(m_numberstack[0]), ARRAY_SIZEOF(m_numberstack));
	stackInit(&m_callStack, m_callstackcontents, sizeof(m_callstackcontents[0]), ARRAY_SIZEOF(m_callstackcontents));
	while (1)
	{
		executeInstruction();
	}
}
