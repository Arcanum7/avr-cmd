/*
 * cmd.c
 *
 *  Created on: 28.08.2013
 *      Author: Grisu
 */
#include <stdio.h>
#include <stdlib.h>
#include <util/delay.h>
#include "usart.h"
#include "string.h"
#include "cmd.h"
#include "config.h"

//this incs, funcs and vars are only needed if SD is in use
#if USE_SD == 1
	#include "sd/mmc_config.h"
	#include "sd/file.h"
	#include <stdarg.h>

	// functions for file operation commands
	static int8_t cmd_execF(void);
	static int8_t cmd_showF(void);
	static int8_t cmd_newF(void);
	static int8_t cmd_writeF(void);
	static int8_t cmd_delF(void);
	static int8_t cmd_lsF(void);
	static int8_t cmd_openF(void);

	// delay and loop commands are only useful in scripts
	static int8_t cmd_delay(void);
	static int8_t cmd_loop(void);
	static int8_t cmd_endLoop(void);
	static int8_t cmd_if(void);
	static int8_t cmd_else(void);
	static int8_t cmd_endif(void);
	static int8_t cmd_while(void);
	static int8_t cmd_endwhile(void);

	// pointer buffer for file argumenst
	char* file_arg_ptr[ARG_BUF];
	char loop_tmp_arg[6];

	uint32_t loop_start = 0, while_start = 0; //file pointer to start of loop

	uint16_t 	loop_cnt = 0, // counter for loops
				loop_cnt_start = 0, //at very beginning of loop save cnt value
									//so we can check if it will be changed in loop
				if_deep = 0, // how deep we are when if is in if is in ....
				if_disc=0; // counter to discard if blocks
						   // if parent if block is not executed

	volatile _Bool 	in_if = FALSE, if_result = FALSE,
					while_result = FALSE, in_while = FALSE;
#endif

// Functions for command line parsing
static void get_arg_from_line(uint8_t position, char* line, char* output);
static int8_t parse_value(char* value, uint16_t* out);
static void uint_to_bin(char* out, uint16_t value);

// functions for general commands
static int8_t cmd_set(void);
static int8_t cmd_print(void);
static int8_t cmd_math(void);

// array of user accessible variables
uint16_t public_vars[VAR_BUF] = {0}, line_len = 0;
// array for pointers to extracted command line arguments
char* arg_ptr[ARG_BUF] = {0};

// This is the command table
	// First value is the string which triggers the command to be executed
	// Second value is a pointer to the corresponding function to the command
COMMAND_STRUCTUR COMMAND_TABLE[] = // Befehls-Tabelle
{
	{"set",cmd_set},
	{"print",cmd_print},
	{"add",cmd_math},
	{"sub",cmd_math},
	{"mul",cmd_math},
	{"div",cmd_math},
	{"mod",cmd_math},
	{"shl",cmd_math},
	{"shr",cmd_math},
	{"and",cmd_math},
	{"or",cmd_math},
	{"xor",cmd_math},
//SD Functions
#if USE_SD == 1
		{"open",cmd_openF},
		{"exec",cmd_execF},
		{"show",cmd_showF},
		{"new",cmd_newF},
		{"del",cmd_delF},
		{"write",cmd_writeF},
		{"ls",cmd_lsF},
		{"delay",cmd_delay},
		{"loop",cmd_loop},
		{"endloop",cmd_endLoop},
		{"if",cmd_if},
		{"else",cmd_else},
		{"endif",cmd_endif},
		{"while",cmd_while},
		{"endwhile",cmd_endwhile},
#endif
	{NULL,NULL} // Marks the end of command table
};


//----------------------------------------------------------------------------------------------------
// parse_line() gets the arguments from the command line and executes the corresponding command
	//
	// line = pointer to command line string, which should be parsed
	//
	// no return
void parse_line(char* line){

	//buffer for extracted commands
	char gr_buf[ARG_TMP_BUF];
	uint8_t i = 0;

	line_len = strlen(line);

	//get arguments from command line
	get_arg_from_line(0,line,gr_buf);
	while(strlen(gr_buf) > 0 && i < ARG_BUF){ // while there is one more arg to parse
		free(arg_ptr[i]);
		// allocate memory for argument and place its pointer in arg_ptr array
		arg_ptr[i] = strcpy(malloc((strlen(gr_buf) + 1) * sizeof(char)),gr_buf);
		i++;
		// get next arg
		get_arg_from_line(i,line,gr_buf);
	}

	//search command in command table, command is arg0
	i = 0;
	while(strcmp(COMMAND_TABLE[i].cmd,arg_ptr[0])){
		//if command not found in table (the NULL end of table has been reached)
		if (COMMAND_TABLE[++i].cmd == 0) {
			printf(ESC_RED"ERR | Unknown Command: %s"ESC_CLEAR,arg_ptr[0]);
			return;
		}
	}

	// when we are not in if execution state or if it's the right if block
	if(!in_if || (in_if && if_result) || !(strcmp(arg_ptr[0],"else")) || !(strcmp(arg_ptr[0],"endif"))){
		//same shit for while
		if(!in_while || (in_while && while_result) || !(strcmp(arg_ptr[0],"endwhile")))
			COMMAND_TABLE[i].fp(); //Exec command
	}
	//are we in an if block that schould not be executed and an if occurs?
	//then we have to count this if to discard its else and endif
	//endif lowers this
	else if(!if_result && !(strcmp(arg_ptr[0],"if")) && in_if)
		if_disc++;

	//free arguments memory and set to '\0'
	for(i = 0; i < ARG_BUF; i++){
		free(arg_ptr[i]);
		arg_ptr[i]=strcpy(malloc(sizeof(char)), "");
	}
}


//----------------------------------------------------------------------------------------------------
// get_arg_from_line() gets a specific argument from the command line
	//
	// arguments must be separated by spaces.
	// strings with spaces could be used by surrounding them by ""
	//
	// position = position of requested arg (0: first arg, 1: second arg ...)
	// line = pointer to command line string
	// output = pointer to output buffer for extracted argument
	//
	// no return
static void get_arg_from_line(uint8_t position, char* line, char* output){
	uint8_t spaces = 0, i = 0, strflag = 0;

	while(line[i] == ' ') { i++; }; // discard leading spaces

	while(line[i] != '\0' && position >= spaces){ //while not at and of line and position not found

		if(line[i] == ' ' && !strflag){ // next position in imaginary arg list
			spaces++;
			while(line[++i] == ' '); // discard repeated spaces
			if(line[i] == '"'){ // detect beginning of a string
				i++;
				strflag = 1;
			}
		}

		// if we are in a string but not at right argument we have to check for EO String
		else if(line[i] == '"' && line[i-1] != '\\' && (position != spaces)){
			strflag = 0;
		}

		// if we are at requested position we get the chars from the command line string
		if(position == spaces){
			if(line[i] == '"' && strflag){ // is it the end of a string or an escape for "
				if(line[i-1] == '\\')
					*(output-1) = '"';
				else
					spaces++; // ends while
			}
			else
				*output++ = line[i];
		}

		i++;

	}
	*output = '\0';
}


//----------------------------------------------------------------------------------------------------
// parse_value() interprets the corresponding numeric value of a string
	//
	// value = pointer to value string, which should be parsed
	// out = pointer to numeric output of conversion
	//
	// returns status of conversion which indicates what type was detected
static int8_t parse_value(char* value, uint16_t* out){

	uint16_t val = 0;
	int8_t status = ERROR;
	char* ptr;

	if((value[0] == '0') && ((value[1] == 'x') || (value[1] == 'b') || (value[1] == 'o'))){	//maybe hex, binary or octal
// HEX
		if(value[1] == 'x'){	// hex
			val = strtoul(value,&ptr,16);
			if(*ptr == '\0')
				status = CONV_H;
		}
// BINARY
		else if(value[1] == 'b'){
			val = strtoul(value + 2,&ptr,2);
			if(*ptr == '\0')
				status = CONV_B;
		}
// OCTAL
		else if(value[1] == 'o'){
			val = strtoul(value + 2,&ptr,8);
			if(*ptr == '\0')
				status = CONV_O;
		}
	}
// ON / OUT
	else if(!strcmp(value,"on") || !strcmp(value,"out")){
		val = 1;
		status = CONV_SW;
	}
//OFF / IN
	else if(!strcmp(value,"off") || !strcmp(value,"in")){
		val = 0;
		status = CONV_SW;
	}
//VAR or REG
	//if p(in), d(dr), o(port), var or arg
	else if(value[0] == 'p' || value[0] == 'd' || value[0] == 'o' || value[0] == '$' || value[0] == '@'){
		uint8_t reg = 0, offset = 4;

		switch(value[0]){		//get port register base address
			case 'p': reg = PIN_REG; break;
			case 'd': reg = DDR_REG; break;
			case 'o': reg = PORT_REG; break;
		}

		offset =  (value[1] - 97);		// calculate register offset depending on given port name (a-d)

		if((reg >= 0) && (offset < 4)){ // if register found and offset is valid
			if(strlen(value) == 3){ // if a pin of a port is adressed
				val = strtoul(value + 2,&ptr,10); // get pin number as numeric value
				if(*ptr == '\0' && val < 8){ // if pin number was valid output value will be set to pin state (1 or 0)
					val = ((_SFR_IO8(reg + (offset * 3))) >> val) & 0x01;
					status = CONV_REG;
				}
			}
			else{ // if the whole register should be read, output value will be set to register value
				val = _SFR_IO8(reg + (offset * 3));
				status = CONV_REG;
			}
		}
		else if(strlen(value) > 1 && value[0] == '$'){ // no port. but var?
			val = strtoul(value + 1,&ptr,10); // get numeric value of var number
			if(*ptr == '\0' && val < VAR_BUF){ // if var number is valid and in bounds of max var count
				val = public_vars[val]; //set output value to value of var
				status = CONV_VA;
			}
		}

// this value conversion is only needed in scripts
#if USE_SD == 1
		else if(strlen(value) > 1 && value[0] == '@'){ // no port, no var. but file_arg?
			val = (uint16_t)strtoul(value + 1,&ptr,10); // get numeric value of file_arg number
			if(*ptr == '\0' && (val + 2) < ARG_BUF){ // if file_arg number is valid and in bounds of max file_arg count
				if(parse_value(file_arg_ptr[val + 2],&val) > ERROR)
					status = CONV_ARG;
			}
		}
#endif
	}
// DECIMAL
	else if(strlen(value) > 0){ // Last try...if the type of value is none of
								// the above types, try to make an integer of it
								// but not when value is empty
		val = strtoul(value,&ptr,10); // get numeric value of value string
		if(*ptr == '\0') // value is a valid number
			status = CONV_D;
	}
	*out = val; // set output to extracted value, 0 when conversion failed
	return status;
}


//----------------------------------------------------------------------------------------------------
// cmd_set() executes the 'set' command. It expects a value in arg1 and arg2
	//
	// arg1 must be a var, a DDR-Reg or a PORT-Reg
	// arg2 must be a var, a Port-Reg or any other valid value corresponding to parse_value()
	//		in scripts arg2 can also be an arg
	//
	// returns status of execution
//TODO: mask out usart pins???
static int8_t cmd_set(void){
	int8_t reg = 0, offset = 4, ret = ERROR;
	uint16_t val = 0, tmp;
	char* ptr;

	//lower args for uniformity
	strlwr(arg_ptr[1]);
	strlwr(arg_ptr[2]);

	// are there possible valid args
	if((strlen(arg_ptr[1]) > 1) && (strlen(arg_ptr[2]) > 0)
			&& parse_value(arg_ptr[2],&val) > ERROR && !(arg_ptr[1][0] == '@')){

		// get port register base address
		switch(arg_ptr[1][0]){
			case 'd': reg = DDR_REG; break;
			case 'p': reg = PORT_REG; break;
		}

		offset =  (arg_ptr[1][1] - 97); // calculate register offset depending on given port name (a-d)

		printf(ESC_YELLOW);

		if((reg > 0) && (offset < 4)){ // if register found and offset is valid -> a port-reg is addressed
			if(strlen(arg_ptr[1]) == 3){ // if a pin of a port is addressed
				tmp = strtoul(arg_ptr[1] + 2,&ptr,10); // get pin number as numeric value
				if(*ptr == '\0' && tmp < 8){ // if pin number was valid
					if(val)
						(_SFR_IO8(reg + (offset * 3))) |= (1 << tmp); // if val says 'set on', set pin high
					else
						(_SFR_IO8(reg + (offset * 3))) &= ~(1 << tmp); // else set pin low

					printf("SET | %s = %u",arg_ptr[1],val);
					ret = 1;
				}
			}
			else{ // if a port is addressed, set port to value
				_SFR_IO8(reg + (offset  *3)) = val;
				printf("SET | %s = %u",arg_ptr[1],val);
				ret = 1;
			}
		}
		else if(strlen(arg_ptr[1]) > 1 && arg_ptr[1][0] == '$'){ // no port? then maybe a var is addressed
			tmp = strtoul(arg_ptr[1] + 1,&ptr,10); // get numeric value of var number
			if(*ptr == '\0' && tmp < VAR_BUF){ // if var number is a valid value
				public_vars[tmp]=val; //save value to user var
				printf("SET | %s = %u",arg_ptr[1],val);
				ret = 1;
			}
		}

	}
// this is only needed in scripts
#if USE_SD== 1
	// no var or port? then maybe a file_arg is addressed
	else if(strlen(arg_ptr[1]) > 1 && arg_ptr[1][0] == '@' && strlen(arg_ptr[2]) > 0
			&& strlen(arg_ptr[2]) <= ARG_TMP_BUF){
		tmp = strtoul(arg_ptr[1] + 1,&ptr,10); // get numeric value of var number
		if(*ptr == '\0' && (tmp + 2) < VAR_BUF){ // if var number is a valid value
			if(!(file_arg_ptr[tmp+2][0] == '\0')){ // only if this file_arg is in use
				//free old memory
				free(file_arg_ptr[tmp+2]);
				// allocate memory for new value and save pointer in file arg list
				file_arg_ptr[tmp+2] = strcpy(malloc((strlen(arg_ptr[2]) + 1) * sizeof(char)),arg_ptr[2]);
				printf("SET | %s = %s",arg_ptr[1],arg_ptr[2]);
				ret = 1;
			}
		}
	}
#endif

	if(!(ret > ERROR)) // no port or var is addressed or you screwed up the value...fu** you...ERROR-Time
		printf(ESC_RED"ERR | %s = %s",arg_ptr[1],arg_ptr[2]);
	printf(ESC_CLEAR);
	return ERROR;
}


//----------------------------------------------------------------------------------------------------
// cmd_print() outputs given values to console
	//
	// Option 1: arg1 is a var and arg2 is a base indicator.
	//			 value of arg1 will be displayed. the type depends on base indicator in arg2.
	//			 in scripts arg1 can be a file_arg (must have a value that is valid for parse_value())
	// Option 2: arg1 is a file_arg (only in scripts)
	// 			 the string content of file_arg will be displayed
	// Option 3: arg1 is not NULL
	//			 the string value of arg1 will be displayed
	//
	// returns status of execution
static int8_t cmd_print(void){
	int8_t ret= ERROR;
	uint16_t tmp;

	//is there a possible valid var or file_arg and base indicator
	if(((arg_ptr[1][0] == '@') || (arg_ptr[1][0] == '$'))
			&& (strlen(arg_ptr[2])==1) && (parse_value(arg_ptr[1],&tmp) > ERROR)){

		// lower base indicator for uniformity
		strlwr(arg_ptr[2]);
		printf(ESC_YELLOW);
		switch(arg_ptr[2][0]){ //display value of arg1 corresponding to base indicator
			case 'i':
				printf("PRINT | %s = %u",arg_ptr[1],tmp);
				ret = 1;
				break;
			case 'x':
				printf("PRINT | %s = 0x%x",arg_ptr[1],tmp);
				ret = 1;
				break;
			case 'o':
				printf("PRINT | %s = 0o%o",arg_ptr[1],tmp);
				ret = 1;
				break;
			case 'b':{
				char illuminati_out[23];
				uint_to_bin(illuminati_out,tmp); // some magical interpretation of value to get a binary string
				printf("PRINT | %s = %s",arg_ptr[1],illuminati_out);
				ret = 1;
				break;
			}
			default:
				 //ALL YOUR BASE ARE BELONG TO US
				printf(ESC_RED"PRINT | PRINT: '%s' wrong base",arg_ptr[2]); // wrong base
				ret = ERROR;
				break;
		}
	}

// when SD is in use check for file_arg
#if USE_SD == 1
	else if((arg_ptr[1][0] == '@') && (strlen(arg_ptr[1]) >1)
			&& (parse_value((arg_ptr[1] + 1),&tmp) > ERROR)){
		//if its an file_arg print its string
		printf(ESC_YELLOW"%s"ESC_YELLOW""CRLF,file_arg_ptr[tmp+2]);
		ret=1;
	}
#endif
	else if(strlen(arg_ptr[1]) > 0){ // there is something oter in arg1? then show it as string
		printf(ESC_YELLOW"%s"ESC_YELLOW""CRLF,arg_ptr[1]);
		ret=1;
	}
	if(!(ret > ERROR))// nothing to display...fu** you...ERROR-Time
		printf(ESC_RED"ERR |  PRINT: no value");
	printf(ESC_CLEAR);
	return ret;
}


//----------------------------------------------------------------------------------------------------
//Begin of SD functions section, only needed when SD file system is used
#if USE_SD == 1

//----------------------------------------------------------------------------------------------------
// cmd_execF() executes the content of a given file line by line where each line is a command line input
	//
	// arg1 must be a valid filename
	//
	// returns status of execution
static int8_t cmd_execF(void){
	uint32_t seek, tmp_pos;
	uint8_t i;

	if( MMC_FILE_OPENED == ffopen((uint8_t*)arg_ptr[1],'r') ){ // try to open file
		seek = file.length;

		printf(ESC_YELLOW"OPEN | %s ... "ESC_GREEN"OK"ESC_CLEAR""CRLL,arg_ptr[1]);

		if(seek > 0){ // if file is not empty
			for(i = 0; i < ARG_BUF; i++) // copy args to file_arg buffer for use in script
				file_arg_ptr[i] = strcpy(malloc((strlen(arg_ptr[i])+1)*sizeof(char)),arg_ptr[i]);

			char line_buf[BUFFER_SIZE] = {0};
			uint8_t cnt = 0;
			do{ //do while not EOF
				do{ //do while not EOF and line buffer is not full
					line_buf[cnt++] = ffread();  //get char from file to line buffer
						if(line_buf[cnt-1] == '\r'){ // if end of line
							line_buf[cnt-1] = '\0';
							ffread(); //discard newline char
							seek--;
							tmp_pos = file.seek + file.cntOfBytes; // save position in file
							printf(ESC_CYAN"  > %s"CRLF"    ",line_buf);
							parse_line(line_buf); // execute command line in line buffer
							printf(CRLF);

							//if cmd_endLoop() resets the position to start of loop
							if(tmp_pos > (file.seek + file.cntOfBytes))
								// we must rewind seek to prevent file from closing to early
								seek += (tmp_pos - (file.seek + file.cntOfBytes));
							cnt = 0;
							line_buf[cnt] = '\0';
						}
				}while(--seek && (cnt < BUFFER_SIZE)); //do while not EOF and line buffer is not full
				cnt = 0;  // line buffer overflof, discard buffer and reset to 0 then input will be continued
				line_buf[cnt] = '\0';
			}while(seek);  //do while not EOF
		}
		printf(CRLF""ESC_YELLOW"CLOSE | %s ... "ESC_GREEN"OK"ESC_CLEAR""CRLF,file_arg_ptr[1]);
		ffclose(); // close file
		file_args_init(); // free file_arg buffer
		return 1;
	}
	// file not found...fu** you...ERROR-Time
	printf(ESC_RED"ERR |  Can't open %s"ESC_CLEAR,arg_ptr[1]);
	return ERROR;
}


//----------------------------------------------------------------------------------------------------
// cmd_showF() prints the content of a file
	//
	// arg1 must be a valid filename
	//
	// returns status of execution
static int8_t cmd_showF(void){
	uint32_t seek;
	if( MMC_FILE_OPENED == ffopen((uint8_t*)arg_ptr[1],'r') ){ // if file can be opened
		seek = file.length;
		printf(ESC_YELLOW"OPEN | %s ... "ESC_GREEN"OK"ESC_CLEAR""CRLL,arg_ptr[1]);
		if(seek > 0){  // if file is not empty
			do{
				uart_putc(ffread(),NULL);  // output file content

			}while(--seek); // uintill EOF
		}
		printf(CRLF""ESC_YELLOW"CLOSE | %s ... "ESC_GREEN"OK"ESC_CLEAR""CRLF,file_arg_ptr[1]);
		ffclose(); // close file
		return 1;
	}
	// file not found...fu** you...ERROR-Time
	printf(ESC_RED"ERR |  Can't open %s"ESC_CLEAR,arg_ptr[1]);
	return ERROR;
}


//----------------------------------------------------------------------------------------------------
// cmd_newF() creates a new empty file
	//
	// arg1 must be a valid filename
	//
	// returns status of execution
static int8_t cmd_newF(void){
	if(MMC_FILE_CREATED == ffopen((uint8_t*)arg_ptr[1],'c') ){ // try to create file
		printf(ESC_YELLOW"NEW | %s ... ",arg_ptr[1]);
//		ffwrite(0x00); // write a terminating char so file is "empty" but not empty
		ffclose(); // close file
		printf(ESC_GREEN"OK"ESC_CLEAR""CRLF);
		return 1;
	}
	// can't create file...fu** you...ERROR-Time
	printf(ESC_RED"ERR |  Can't create %s"ESC_CLEAR,arg_ptr[1]);
	return ERROR;
}


//----------------------------------------------------------------------------------------------------
// cmd_writeF() writes the string in arg2 to a file as a new line
	//
	// arg1 must be a valid filename
	// arg2 must be a string
	//
	// returns status of execution
static int8_t cmd_writeF(void){
	if((strlen(arg_ptr[1]) > 0) && (strlen(arg_ptr[2]) > 0)){ // is there a filename and a string to write
		if(MMC_FILE_OPENED == ffopen((uint8_t*)arg_ptr[1],'r') ){ // try to open file
			printf(ESC_YELLOW"WRITE | %s ... ",arg_ptr[1]);
			ffseek(file.length); // set file pointer to EOF
		   	ffwrites((uint8_t*)arg_ptr[2]); // put string of arg2 to file
			ffwrite(0x0D);		// new line in file
		   	ffwrite(0x0A);
			ffclose();	//close file
			printf(ESC_GREEN"OK"ESC_CLEAR""CRLF);
			return 1;
		}
	}
	// file not found...fu** you...ERROR-Time
	printf(ESC_RED"ERR |  Can't write to %s"ESC_CLEAR,arg_ptr[1]);
	return ERROR;
}


//----------------------------------------------------------------------------------------------------
// cmd_loop() starts loop which executes loop block arg1 times
	//runs one time even when value is 0 or not valid
	// arg1 must be a valid value for parse_value
	//
	// returns status of execution
static int8_t cmd_loop(void){
	if((parse_value(arg_ptr[1],&loop_cnt) > ERROR)){ // get numeric value of arg1
		// do we have a var?
		if(((arg_ptr[1][0] == '@') || (arg_ptr[1][0] == '$')) ){
			strcpy(loop_tmp_arg,arg_ptr[1]); //save for later to check if it changes inside the loop
			loop_cnt_start = loop_cnt;
		}
		else
			loop_tmp_arg[0] = '\0';
		if(loop_cnt) //when value is > 0 we have to decrement to execute exact number
			loop_cnt--;
		loop_start = file.seek + file.cntOfBytes; // save position in file to restart the loop

		return 1;
	}
	loop_cnt=0;
	// no valid value...fu** you...ERROR-Time
	printf(ESC_RED"ERR |  No valid value for Loop %s"ESC_CLEAR,arg_ptr[1]);
	return ERROR;
}


//----------------------------------------------------------------------------------------------------
// cmd_endLoop() indicates the end of a loop block and handles restart of loop
	//
	// no input
	//
	// returns status of execution
static int8_t cmd_endLoop(void){
	uint16_t tmp;
	if(!(loop_tmp_arg[0] == '\0')) //did we have a var as loop
		if( parse_value(loop_tmp_arg,&tmp) > ERROR) //can we get a value of it?
			if(loop_cnt_start != tmp) // has it changed?
				(tmp > 0)?(loop_cnt = tmp - 1):(loop_cnt = 0);

	if(loop_cnt-- > 0){ //if there are some executions left
		ffseek(loop_start); // set back file pointer to first command of loop
	}
	return 1;
}


//----------------------------------------------------------------------------------------------------
// cmd_if() indicates the start of a if and executes the if statement
	// see also parse_line() to understand
	//
	// no input
	//
	// returns status of execution
static int8_t cmd_if(void){
	uint16_t tmp1,tmp2;
	//can we get a real value of the two given values
	if((parse_value(arg_ptr[1],&tmp1) > ERROR) && (parse_value(arg_ptr[3],&tmp2) > ERROR)){
		switch(arg_ptr[2][0]){
		case '>':
			if_result = (tmp1 > tmp2)?TRUE:FALSE;
			in_if = TRUE;
			if_deep++;
			return 1; break;
		case '<':
			if_result = (tmp1 < tmp2)?TRUE:FALSE;
			in_if = TRUE;
			if_deep++;
			return 1; break;
		case '=':
			if_result = (tmp1 == tmp2)?TRUE:FALSE;
			in_if = TRUE;
			if_deep++;
			return 1; break;
		case '!':
			if_result = (tmp1 != tmp2)?TRUE:FALSE;
			in_if = TRUE;
			if_deep++;
			return 1; break;
		}
	}
	// no valid value...fu** you...ERROR-Time
	printf(ESC_RED"ERR |  a non valid value occurs: %s %s %s"ESC_CLEAR,arg_ptr[1],arg_ptr[2],arg_ptr[3]);
	return ERROR;
	return 1;
}


//----------------------------------------------------------------------------------------------------
// cmd_else() indicates the else statement of a if block
	// see also parse_line() to understand
	//
	// no input
	//
	// returns status of execution
static int8_t cmd_else(void){
	if(in_if && if_disc == 0) //are we in if and should we not discard it
							  //(see parse_line() for if_disc)
		if_result = (if_result) ? FALSE : TRUE;

	return 1;
}


//----------------------------------------------------------------------------------------------------
// cmd_endif() indicates the end of a if block
	// see also parse_line() to understand
	//
	// no input
	//
	// returns status of execution
static int8_t cmd_endif(void){
	if(if_disc == 0){ //discard (see parse_line())?
		if(--if_deep == 0) // all if blocks closed?
			in_if = FALSE;
		else
			if_result = TRUE; //by setting TRUE, the rest of parent loop
							  //will be executed
	}
	else
    	if_disc--;

	return 1;
}


//----------------------------------------------------------------------------------------------------
// cmd_while() indicates the else statement of a if block
	// see also parse_line() to understand
	//
	// no input
	//
	// returns status of execution
static int8_t cmd_while(void){
	// safe if values. we will reuse if for while
	// and have to restore its values
	_Bool tmp1 = if_result, tmp2 = in_if;
	uint16_t tmp3 = if_deep;

	if(cmd_if() > ERROR){

		while_result = if_result; // copy if values
		in_while = in_if;		  // to corresponding while values

		if(while_result) // if we execute the block, we will come back later
			// save position in file to restart the loop
			while_start = file.seek + file.cntOfBytes -line_len - 2;
	}
	if_result = tmp1; // restore if values
	in_if = tmp2;
	if_deep = tmp3;
	return 1;
}


//----------------------------------------------------------------------------------------------------
// cmd_endwhile() indicates the end statement of a while block
	// see also parse_line() to understand
	//
	// no input
	//
	// returns status of execution
static int8_t cmd_endwhile(void){
		if(while_result) //when this while loop was executed
			// set back file pointer to while line (beginning of while block)
			ffseek(while_start);
		in_while = FALSE;

	return 1;
}


//----------------------------------------------------------------------------------------------------
// cmd_delF() deletes a given file
	//
	// arg1 must be a valid filename
	//
	// returns status of execution
static int8_t cmd_delF(void){
	if(ffrm((uint8_t*)arg_ptr[1])){ // when deleting was successful
		printf(ESC_YELLOW"DEL | %s ... "ESC_GREEN"OK"ESC_CLEAR""CRLF,arg_ptr[1]);
		return 1;
	}
	// file not found...fu** you...ERROR-Time
	printf(ESC_RED"ERR |  Can't delete %s"ESC_CLEAR,arg_ptr[1]);
	return ERROR;
}


//----------------------------------------------------------------------------------------------------
// cmd_lsF() prints a list of directory contents
	//
	// no input
	//
	// returns status of execution
static int8_t cmd_lsF(void){
		ffls();
		return 1;
}


//----------------------------------------------------------------------------------------------------
// cmd_openF() prints the content of a file then waits for input which will
	// be appended to file. End this mode by typing '%exit'
	//
	// arg1 must be a valid filename
	//
	// returns status of execution
static int8_t cmd_openF(void){
	uint32_t seek;
	if( MMC_FILE_OPENED == ffopen((uint8_t*)arg_ptr[1],'r') ){ // if file can be opened
		seek = file.length;
		printf(ESC_YELLOW"OPEN | %s ... "ESC_GREEN"OK"ESC_CLEAR""CRLL,arg_ptr[1]);
		if(seek > 0){ // if file is not empty
			do{
				uart_putc(ffread(),NULL);  // output file content
			}while(--seek); // uintill EOF
		}
		while(1){
			usart_status.usart_ready = 0;
			printf("  > ");
			while(usart_status.usart_ready==0); // wait for line input
			if(strcmp(usart_rx_buffer,"%exit")){ // if not exit command
				ffwrites((uint8_t*)usart_rx_buffer); //write line to file
				ffwrite('\r');
				ffwrite('\n');
				printf(CR""ESC_CLRL"%s"CRLF,usart_rx_buffer);
			}
			else //if exit command, break
				break;
		}
		printf(CRLF""ESC_YELLOW"CLOSE | %s ... "ESC_GREEN"OK"ESC_CLEAR""CRLF,file_arg_ptr[1]);
		ffclose(); // close file
		return 1;
	}

	// file not found...fu** you...ERROR-Time
	printf(ESC_RED"ERR |  Can't open %s"ESC_CLEAR,arg_ptr[1]);
	return ERROR;
}


//----------------------------------------------------------------------------------------------------
// cmd_delay() does nothing for a given while
	//
	// arg 1 must be m for milliseconds or u for microseconds
	// arg 2 must be a valid numeric value for parse_value()
	//
	// returns status of execution
static int8_t cmd_delay(void){
	uint16_t parsedval = 0;
	int8_t status = parse_value(arg_ptr[2],&parsedval);  // get numeric value of arg2

	if((arg_ptr[1][0] == 'm') && (status > ERROR)){ // if ms is choosen and arg2 is valid numeric value
		printf(ESC_YELLOW"DELAY |  %ss = %u"ESC_CLEAR,arg_ptr[1],parsedval);
		for(uint16_t i = 0; i < parsedval; i++) // delay 1ms until time is over
			_delay_ms(1);
		return 1;

	}
	else if((arg_ptr[1][0] == 'u') && (status > ERROR)){ // if us is choosen and arg2 is valid numeric value
		printf(ESC_YELLOW"DELAY |  %ss = %u"ESC_CLEAR,arg_ptr[1],parsedval);
		for(uint16_t i = 0; i < parsedval; i++) // delay 1us until time is over
			_delay_us(1);
		return 1;
	}
	// no valid input...fu** you...ERROR-Time
	printf(ESC_RED"ERR |  %s = %i"ESC_CLEAR,arg_ptr[2],status);
	return ERROR;
}


//----------------------------------------------------------------------------------------------------
// file_args_init() initializes or frees the file_arg buffer
	//
	// no input
	//
	// no return
// DONE: stop at empty file_arg
void file_args_init(void){
	for(uint16_t i = 0; i < ARG_BUF; i++){ // for each file_arg in buffer
		if(file_arg_ptr[i][0] == '\0') // if file_arg is empty
			break; // we can stop. all buffers are empty now
		else{
			free(file_arg_ptr[i]); // free memory
			file_arg_ptr[i]=strcpy(malloc(sizeof(char)), ""); // set file_arg i to empty string
		}
	}
}

//----------------------------------------------------------------------------------------------------
//End of SD functions section
#endif


//----------------------------------------------------------------------------------------------------
// math() executes the math commands
	//
	// arg1 must be a var
	// arg2 must be a var or valid numeric input value for parse_value()
	//
	// returns status of execution
// DONE: check for valid arg number
static int8_t cmd_math(void){
	uint16_t tmp1,tmp2,tmp3;
	char* c = "";

	//if arg1 and arg2 have valid numeric values and arg1 is a var
	if((parse_value(arg_ptr[1],&tmp1) > ERROR) && (parse_value(arg_ptr[2],&tmp2) > ERROR)
			&& (arg_ptr[1][0] == '$') && (parse_value(arg_ptr[1]+1,&tmp3) > ERROR)){
		if(tmp3 < VAR_BUF){
			switch(arg_ptr[0][0]){ //decide which operation should be executed
				case 'a' :
					if(arg_ptr[0][1] == 'd'){
						public_vars[tmp3] = tmp1 + tmp2; // add value of arg2 to value of arg1 and save to arg1
						c = "+";
					}
					else{
						public_vars[tmp3] = tmp1 & tmp2; // bitwise and...
						c = "&";
					}
					break;
				case 's' :
					if(arg_ptr[0][2] == 'l'){
						public_vars[tmp3] = tmp1 << tmp2; //shift l
						c = "<<";
					}
					else if(arg_ptr[0][2] == 'r'){
						public_vars[tmp3] = tmp1 >> tmp2; //shift r
						c = ">>";
					}
					else{
						public_vars[tmp3] = tmp1 - tmp2; // Subtract
						c = "-";
					}
					break;
				case 'm' :
					if(arg_ptr[0][1] == 'u'){
						public_vars[tmp3] = tmp1 * tmp2; // multiply
						 c = "*";
					}
					else{
						public_vars[tmp3] = tmp1 % tmp2; // modulo
						 c = "%";
					}
					break;
				case 'd' : public_vars[tmp3] = tmp1 / tmp2; c = "/"; break; // divide
				case 'o' : public_vars[tmp3] = tmp1 | tmp2; c = "|"; break; // bitwise or
				case 'x' : public_vars[tmp3] = tmp1 ^ tmp2; c = "^"; break; // bitwise xor
			}
			printf(ESC_YELLOW"%s |  %s %s %s = %u"ESC_CLEAR,strupr(arg_ptr[0]),arg_ptr[1],c,arg_ptr[2],public_vars[tmp3]);
			return 1;
		}
	}
	// fufufufufufufufu
	printf(ESC_RED"ERR |  %s %s %s"ESC_CLEAR,arg_ptr[1],arg_ptr[0],arg_ptr[2]);
	return ERROR;
}


//----------------------------------------------------------------------------------------------------
// uint_to_bin() does some magical stuff to convert a unit16 to a binary string
	//
	// out = pointer to string output buffer
	// value = the value which should be converted
	//
	// no return
void uint_to_bin(char* out, uint16_t value){
	uint8_t i = 0;
	char* h = out;
	uint8_t m = 12;
	if(value > 0xF)
		m = 8;
	if(value > 0xFF)
		m = 4;
	if(value > 0xFFF)
		m = 0;
	*h++ = '0';
	*h++ = 'b';
	do{
		if(i>0 && !(i%4))
			*h++ = '.';
		uint8_t t = ((value << ((i++*1)+m)) & 0x8000) >> 15;
		*h++ = t + 0x30;
	}while(i<(16-m));
	*h = '\0';
}
