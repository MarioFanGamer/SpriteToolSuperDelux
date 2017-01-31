#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <cctype>
#include <dirent.h>
#include "asar/asardll.h"

//10 per leve, 200 level + F0 global
#define MAX_SPRITE_COUNT 0x20F0
//use 16MB ROM size to avoid asar malloc/memcpy on 8MB of data per block. 
#define MAX_ROM_SIZE 16*1024*1024

#define ROUTINES 0
#define SPRITES 1
#define GENERATORS 2
#define SHOOTERS 3
#define LIST 4

#define RTL_BANK 0x01
#define RTL_HIGH 0x80
#define RTL_LOW 0x21

#define TEMP_SPR_FILE "spr_temp.asm"

struct simple_string{
	int length = 0;
	char *data = nullptr;
	simple_string() = default;
	simple_string(const simple_string&) = default;
	
	simple_string &operator=(simple_string &&move)
	{
		delete []data;
		data = move.data;
		move.data = nullptr;
		length = move.length;
		return *this;
	}
	~simple_string()
	{
		delete []data;
	}
};

template <typename ...A>
void error(const char *message, A... args)
{
	printf(message, args...);
	exit(-1);
}

void double_click_exit()
{
	getc(stdin); //Pause before exit
}

FILE *open(const char *name, const char *mode) {
	FILE *file = fopen(name, mode);
	if(!file){
		error("Could not open \"%s\"\n", name);
	}
	return file;
}

int file_size(FILE *file) {
	fseek(file, 0, SEEK_END);
	int size = ftell(file);
	fseek(file, 0, SEEK_SET);
	return size;
}

unsigned char *read_all(const char *file_name, bool text_mode = false, unsigned int minimum_size = 0) {
	FILE *file = open(file_name, "rb");
	unsigned int size = file_size(file);
	unsigned char *file_data = new unsigned char[(size < minimum_size ? minimum_size : size) + (text_mode * 2)]();
	if(fread(file_data, 1, size, file) != size){
		error("%s could not be fully read.  Please check file permissions.", file_name);
	}
	fclose(file);
	return file_data;
}

void write_all(unsigned char *data, const char *file_name, unsigned int size)
{
	FILE *file = open(file_name, "wb");
	if(fwrite(data, 1, size, file) != size){
		error("%s could not be fully written.  Please check file permissions.", file_name);
	}
	fclose(file);
}

int get_pointer(unsigned char *data, int address, int size = 3, int bank = 0x00) {
	address = (data[address])
		| (data[address + 1] << 8)
		| ((data[address + 2] << 16) * (size-2));
	return address | (bank << 16);
}


struct pointer {
	unsigned char lowbyte = RTL_LOW;		//point to RTL
	unsigned char highbyte = RTL_HIGH;	//
	unsigned char bankbyte = RTL_BANK;	//
	
	pointer() = default;
	pointer(int snes) {
		lowbyte = (unsigned char)(snes & 0xFF);
		highbyte = (unsigned char)((snes >> 8) & 0xFF);
		bankbyte = (unsigned char)((snes >> 16) & 0xFF);
	}	
	pointer(const pointer&) = default;
	~pointer() = default;
	
	bool is_empty() {
		return lowbyte == RTL_LOW && highbyte == RTL_HIGH && bankbyte == RTL_BANK;
	}
	
	int addr() {
		return (bankbyte << 16) + (highbyte << 8) + lowbyte;
	}
};

struct ROM{
	unsigned char *data;
	unsigned char *real_data;
	char *name;
	int size;
	int header_size;
	
	void open(const char *n)
	{
		name = new char[strlen(n)+1]();
		strcpy(name, n);
		FILE *file = ::open(name, "r+b");	//call global open
		size = file_size(file);
		header_size = size & 0x7FFF;
		size -= header_size;
		data = read_all(name, false, MAX_ROM_SIZE + header_size);
		fclose(file);
		real_data = data + header_size;
	}
	
	void close()
	{
		write_all(data, name, size + header_size);
		delete []data;
		delete []name;
	}
	
	int pc_to_snes(int address)
	{
		address -= header_size;
		return ((((address << 1) & 0x7F0000) | (address&0x7FFF)) | 0x8000);
	}

	int snes_to_pc(int address)
	{
		return ((address & 0x7F0000) >> 1 | (address & 0x7FFF)) + header_size;
	}
	
	pointer pointer_snes(int address, int size = 3, int bank = 0x00)
	{
		return pointer(::get_pointer(data, snes_to_pc(address), size, bank));
	}
	pointer pointer_pc(int address, int size = 3, int bank = 0x00)
	{
		return pointer(::get_pointer(data, address, size, bank));
	}
};


simple_string get_line(const char *text, int offset){
	simple_string string;
	if(!text[offset]){
		return string;
	}
	string.length = strcspn(text+offset, "\r\n")+1;	
	string.data = new char[string.length]();
	strncpy(string.data, text+offset, string.length-1);
	return string;
}

char *trim(char *text)
{
	while(isspace(*text)){		//trim front
		text++;
	}
	for(int i = strlen(text); isspace(text[i-1]); i--){	//trim back
		text[i] = 0;
	}
	return text; 
}






// 00: type {0=tweak,1=custom,3=generator/shooter}
// 01: "acts like"
// 02-07: tweaker bytes
// 08-10: init pointer
// 11-13: main pointer
// 14: extra property byte 1
// 15: extra property byte 2
struct sprite_table {
	unsigned char type = 0;
	unsigned char actlike = 0;
	unsigned char tweak[6] = {0};
	pointer init;
	pointer main;
	unsigned char extra[2] = {0};
};

struct sprite {
	int line = 0;
	int number = 0;
	int level = 0x200;
	sprite_table* table = nullptr;
	char* asm_file = nullptr;
	char* cfg_file = nullptr;
	const char* description = nullptr;
	
	~sprite() {
		if(asm_file)
			delete[] asm_file;
		if(cfg_file)
			delete[] cfg_file;
		if(description)
			delete[] description;
	}
};

struct sprite_data {	
	sprite_table full_table[MAX_SPRITE_COUNT];
	
	sprite_table* default_table = full_table + 0x2000;		//00-AF = sprite, C0-CF = shooter, D0-FF = gen		
	
	sprite_table* level_table_t1 = full_table;				//sprite B0-BF of level 000-07F
	sprite_table* level_table_t2 = full_table + 0x800;		//sprite B0-BF of level 080-0FF
	sprite_table* level_table_t3 = full_table + 0x1000;	//sprite B0-BF of level 100-17F
	sprite_table* level_table_t4 = full_table + 0x1800;	//sprite B0-BF of level 180-1FF
};

bool is_Empty_Table(sprite_table* ptr, int size) {
	for(int i = 0; i < size; i++)
		if(!ptr[i].init.is_empty() || !ptr[i].init.is_empty())
			return false;
	return true;
}

template <typename T>
T* from_table(T* table, int level, int number) {
	if(level > 0x200 || number > 0xFF)
		return nullptr;
	if(level == 0x200) {
		if(number < 0xB0)
			return table + (0x2000 + number);
		else if(number >= 0xB0 && number < 0xC0)
			return nullptr;
		else
			return table + (0x2000 + number - 0x10);
	}
	else if(number >= 0xB0 && number < 0xC0){
		return table + ((level * 0x10) + (number - 0xB0));
	}
	return nullptr;
}

bool patch(const char *patch_name, ROM &rom, const char *debug_name) {
	if(!asar_patch(patch_name, (char *)rom.real_data, MAX_ROM_SIZE, &rom.size)){
		int error_count;
		const errordata *errors = asar_geterrors(&error_count);
		printf("An error has been detected:\n");
		for(int i = 0; i < error_count; i++)
			printf("%s\n", errors[i].fullerrdata);
			exit(-1);
	}
	return true;
}

void set_pointer(pointer* p, int address) {
	p->lowbyte = (char)(address & 0xFF);
	p->highbyte = (char)((address >> 8) & 0xFF);
	p->bankbyte = (char)((address >> 16) & 0xFF);
}

void patch_sprites(sprite* sprite_list, ROM &rom, bool debug) {
		
	for(int i = 0; i < MAX_SPRITE_COUNT; i++) {
					
		sprite* spr = sprite_list + i;
		if(!spr->asm_file)
			continue;
			
		bool duplicate = false;
		for(int j = i - 1; j >= 0; j--) {
			if(sprite_list[j].asm_file) {
				if(!strcmp(spr->asm_file, sprite_list[j].asm_file)) {
					spr->table->init = sprite_list[j].table->init;
					spr->table->main = sprite_list[j].table->main;
					spr->description = sprite_list[j].description;
					duplicate = true;
					break;
				}			
			}
		}
		
		if(duplicate)
			continue;
			
		FILE* sprite_patch = open(TEMP_SPR_FILE,"w");
		fprintf(sprite_patch, "incsrc \"asm/sa1def.asm\"\n");
		fprintf(sprite_patch, "incsrc \"asm/shared.asm\"\n");
		fprintf(sprite_patch, "freecode cleaned\n");
		fprintf(sprite_patch, "\tincsrc \"%s\"", spr->asm_file);
		fclose(sprite_patch);
					
		patch(TEMP_SPR_FILE, rom, TEMP_SPR_FILE);
			
		int print_count = 0;
		const char * const * prints = asar_getprints(&print_count);
		int addr = 0xFFFFFF;
		char buf[5];
		
		for(int i = 0; i < print_count; i++) {
			sscanf(prints[i], "%4s%x", buf, &addr);			
			if(!strcmp(buf,"INIT"))
				set_pointer(&spr->table->init, addr);
			else if(!strcmp(buf,"MAIN"))
				set_pointer(&spr->table->main, addr);
			else
				spr->description = prints[i];
		}
		
		if(debug) {
			printf("%s\n\tINIT: $%06X\n\tMAIN: $%06X"
				"\n__________________________________\n",
				spr->asm_file,
				spr->table->init.addr(), spr->table->main.addr());
		}
	}
}

typedef void (*linehandler)(const char*, sprite*, void*);

void read_cfg_file(sprite* spr, const char* dir, bool debug) {

	linehandler handlers[6];

	int bytes_read = 0;
	simple_string current_line;
			
	char* cfg = (char *)read_all(spr->cfg_file, true);
		
	sscanf(cfg, "%x %x %x %x %x %x %x %x %x %x%n", 
		&spr->table->type,
		&spr->table->actlike, 
		&spr->table->tweak[0], &spr->table->tweak[1], &spr->table->tweak[2],
		&spr->table->tweak[3], &spr->table->tweak[4], &spr->table->tweak[5],
		&spr->table->extra[0], &spr->table->extra[1],
		&bytes_read);
	
	do{
		current_line = static_cast<simple_string &&>(get_line(cfg, bytes_read));
		bytes_read += current_line.length;
		if(!current_line.length || !trim(current_line.data)[0]){
			continue;
		}

		spr->asm_file = new char[strlen(dir) + current_line.length];		
		strcpy(spr->asm_file, dir);
		strcat(spr->asm_file, current_line.data);
		
	}while(!spr->asm_file);
	
	
	if(debug){	
		if(spr->level < 0x200)
			printf("Sprite: %02X, Level: %03X\n", spr->number, spr->level);
		else
			printf("Sprite: %02X\n", spr->number);
		printf("Type: %02X, ActLike: %02X\nTweaker: ",spr->table->type, spr->table->actlike);
		for(int i = 0; i < 6; i++)
			printf("%02X, ",spr->table->tweak[i]);
		printf("\nExtra: ");
		for(int i = 0; i < 2; i++)
			printf("%02X, ",spr->table->extra[i]);
		printf("\nASM File: %s\n\n", spr->asm_file);
	}	
}


void clean_hack(ROM &rom)
{
	if(!strncmp((char*)rom.data + rom.snes_to_pc(0x02FFE2), "STSD", 4)){		//already installed load old tables				
		FILE* clean_patch = open("asm/cleanup.asm", "w");
		
		//remove per level sprites
		for(int bank = 0; bank < 4; bank++) {
			int level_table_address = (rom.data[rom.snes_to_pc(0x02FFEA + bank)] << 16) + 0x8000;
			for(int table_offset = 8; table_offset < 0x8000; table_offset += 0x10)	{
				pointer init_pointer = rom.pointer_snes(level_table_address + table_offset);
				if(!init_pointer.is_empty()) {
					fprintf(clean_patch, "autoclean $%06X\n", init_pointer.addr());
				}				
			}
			fprintf(clean_patch, "\n");
		}
		
		//remove global sprites
		int global_table_address = rom.pointer_snes(0x02FFEE).addr();
		for(int table_offset = 8; table_offset < 0xF00; table_offset += 0x10)	{
			pointer init_pointer = rom.pointer_snes(global_table_address + table_offset);
			if(!init_pointer.is_empty()) {
				fprintf(clean_patch, "autoclean $%06X\n", init_pointer.addr());
			}				
		}
		
		fprintf(clean_patch, "\n\n");
						
		//shared routines
		for(int i = 0; i < 100; i++){
			int routine_pointer = rom.pointer_snes(0x03E05C + i * 3).addr();
			if(routine_pointer != 0xFFFFFF){
				fprintf(clean_patch, "autoclean $06%X\n", routine_pointer);
				fprintf(clean_patch, "ORG $%06X\n", 0x03E05C + i * 3);
				fprintf(clean_patch, "dl $FFFFFF\n");
			}
		}
		
		fclose(clean_patch);
		patch("asm/cleanup.asm", rom, "asm/cleanup.asm");
		
	}else{ //check for old sprite_tool code. (this is annoying)
		
		//removes all STAR####MDK tags
		const char* mdk = "MDK";	//sprite tool added "MDK" after the rats tag to find it's insertions...
		int number_of_banks = rom.size / 0x8000;
		for (int i = 0x10; i < number_of_banks; ++i){ 
			char* bank = (char*)(rom.real_data + i * 0x8000);

			int bank_offset = 8;
			while(1){
				//look for data inserted on previous uses
				
				int offset = bank_offset;
				unsigned int j = 0;
				for(; offset < 0x8000; offset++) {
					if(bank[offset] != mdk[j++])
						j = 0;
					if(j == strlen(mdk)) {
						offset -= strlen(mdk) - 1;		//set pointer to start of mdk string
						break;
					}
				}
								
				if(offset >= 0x8000)
					break;		
				bank_offset = offset + strlen(mdk);
				if(strncmp((bank + offset - 8), "STAR", 4))	//check for "STAR"
					continue;
								
				//delete the amount that the RATS tag is protecting
				int size = ((unsigned char)bank[offset-3] << 8)
					+ (unsigned char)bank[offset-4] + 8;
				int inverted = ((unsigned char)bank[offset-1] << 8)
					+ (unsigned char)bank[offset-2];
		 
				if ((size - 8 + inverted) == 0x0FFFF)			// new tag
					size++;
					
				else if ((size - 8 + inverted) != 0x10000){	// (not old tag either =>) bad tag
					char answer;
					int pc = i * 0x8000 + offset - 8 + rom.header_size;
					printf("size: %04X, inverted: %04X\n", size - 8, inverted);
					printf("Bad sprite_tool RATS tag detected at $%06X / 0x%05X. Remove anyway (y/n) ",
						rom.pc_to_snes(pc), pc);
					scanf("%c",&answer);
					if(answer != 'Y' && answer != 'y')
						continue;
				}
				
				//printf("Clear %04X bytes from $%06X / 0x%05X.\n", size, rom.pc_to_snes(pc), pc);
				memset(bank + offset - 8, 0, size);
				bank_offset = offset - 8 + size;
			}
		}		
	}
}

void create_shared_patch(const char *routine_path, ROM &rom)
{
	FILE *shared_patch = open("asm/shared.asm", "w");
	fprintf(shared_patch, 	"macro include_once(target, base, offset)\n"
				"	if !<base> != 1\n"
				"		!<base> = 1\n"
				"		pushpc\n"
				"		if read3(<offset>*3+$03E05C) != $FFFFFF\n"
				"			<base> = read3(<offset>*3+$03E05C)\n"
				"		else\n"
				"			freecode cleaned\n"
				"			<base>:\n"
				"			incsrc <target>\n"
				"			ORG <offset>*3+$03E05C\n"
				"			dl <base>\n"				
				"		endif\n"
				"		pullpc\n"
				"	endif\n"
				"endmacro\n");
	DIR *routine_directory = opendir(routine_path);
	dirent *routine_file = nullptr;
	if(!routine_directory){
		error("Unable to open the routine directory \"%s\"\n", routine_path);
	}
	int routine_count = 0;
	while((routine_file = readdir(routine_directory)) != NULL){
		char *name = routine_file->d_name;
		if(!strcmp(".asm", name + strlen(name) - 4)){
			if(routine_count > 100){
				closedir(routine_directory);
				error("More than 100 routines located.  Please remove some.\n", "");
			}
			name[strlen(name) - 4] = 0;
			fprintf(shared_patch, 	"!%s = 0\n"
						"macro %s()\n"
						"\t%%include_once(\"%s%s.asm\", %s, $%.2X)\n"
						"\tJSL %s\n"
						"endmacro\n", 
						name, name, routine_path, 
						name, name, routine_count*3, name);
			routine_count++;
		}
	}
	closedir(routine_directory);
	printf("%d Shared routines registered in \"%s\"\n", routine_count, routine_path);
	fclose(shared_patch);
}

bool populate_sprite_list(const char** paths, sprite *sprite_list, sprite_table* sprite_tables, const char *list_data, bool debug)
{
	int line_number = 0, i = 0, bytes_read, sprite_id, level;
	const char* dir = nullptr;
	simple_string current_line;
	#define ERROR(S) { delete []list_data; error(S, line_number); }
	do{
		level = 0x200;
		current_line = static_cast<simple_string &&>(get_line(list_data, i));
		i += current_line.length;
		if(list_data[i - 1] == '\r')
			i++;
		line_number++;
		if(!current_line.length || !trim(current_line.data)[0]){
			continue;
		}
		else if(!sscanf(current_line.data, "%x%n", &sprite_id, &bytes_read)){
			ERROR("Error on line %d: Invalid line start.\n");
		}
		if(current_line.data[bytes_read] == ':'){
			sscanf(current_line.data, "%x%*c%hx%n", &level, &sprite_id, &bytes_read);
		}
		
		sprite* spr = from_table<sprite>(sprite_list, level, sprite_id);
		if(!spr) {
			if(sprite_id >= 0x100)
				ERROR("Error on line %d: Sprite number must be less than 0x100");
			if(level == 0x200 && sprite_id >= 0xB0 && sprite_id < 0xC0)
				ERROR("Error on line %d: Sprite B0-BF must be assigned a level. Eg. 105:B0");
			if(level > 0x200)
				ERROR("Error on line %d: Level must range from 000-1FF");
			if(sprite_id >= 0xB0 && sprite_id < 0xC0)
				ERROR("Error on line %d: Only sprite B0-BF must be assigned a level.");
		}
					
		if(spr->line)
				ERROR("Error on line %d: Sprite number already used.");
				
		spr->line = line_number;
		spr->level = level;
		spr->number = sprite_id;
		spr->table = from_table<sprite_table>(sprite_tables, level, sprite_id);
		
		if(sprite_id < 0xC0)
			dir = paths[SPRITES];
		else if(sprite_id < 0xD0)
			dir = paths[SHOOTERS];
		else
			dir = paths[GENERATORS];
		
		if(isspace(current_line.data[bytes_read])){
			char *file_name = trim(current_line.data + bytes_read);
			spr->cfg_file = new char[strlen(dir) + strlen(file_name) + 1];
			strcpy(spr->cfg_file, dir);
			strcat(spr->cfg_file, file_name);
			if(!spr->cfg_file[0]){
				ERROR("Error on line %d: Missing filename.\n");
			}
		}else{
			ERROR("Error on line %d: Missing space or level seperator.\n");
		}
		
		read_cfg_file(spr, dir, debug);
		
		
		
		// printf("Read Sprite on line %d:\n"
			// "\tNumber: %02X, Level: %02X\n"
			// "\tCFG File: %s\n",
			// spr->line, spr->number, spr->level, spr->cfg_file);
		
	}while(current_line.length);
	#undef ERROR

	delete []list_data;
	return true;
}


int main(int argc, char* argv[]) {
					
	ROM rom;
	sprite_data data;
	sprite sprite_list[MAX_SPRITE_COUNT];
	bool debug_flag = false;
	bool keep_temp = false;
		
	//first is version 1.xx, others are preserved
	unsigned char versionflag[4] = { 0x00, 0x00, 0x00, 0x00 };
	
	const char* paths[5];
	paths[ROUTINES] = "routines/";
	paths[SPRITES] = "sprites/";
	paths[SHOOTERS] = "shooters/";
	paths[GENERATORS] = "generators/";
	paths[LIST] = "list.txt";
	
	if(argc < 2){
        atexit(double_click_exit);
    }

	if(!asar_init()){
		error("Error: Asar library is missing, please redownload the tool or add the dll.\n", "");
	}
	for(int i = 1; i < argc; i++){
		if(!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help") ){
			printf("Usage: STSD <options> <ROM>\nOptions are:\n");
			printf("-d\t\tEnable debug output\n");
			printf("-k\t\tKeep debug files\n");
			printf("-l <listpath>\tSpecify a custom list file (Default: list.txt)\n");
			
			printf("-p <sprites>\tSpecify a custom generators directory (Default sprites/)\n");
			printf("-o <shooters>\tSpecify a custom generators directory (Default shooters/)\n");
			printf("-g <generators>\tSpecify a custom generators directory (Default generators/)\n");
			
			printf("-s <sharedpath>\tSpecify a shared routine directory (Default routines/)\n");
			exit(0);
		}else if(!strcmp(argv[i], "-d") || !strcmp(argv[i], "--debug")){
			debug_flag = true;
		}else if(!strcmp(argv[i], "-k")){
			keep_temp = true;
		}else if(!strcmp(argv[i], "-s") && i < argc - 2){
			paths[ROUTINES] = argv[i+1];
			i++;
		}else if(!strcmp(argv[i], "-p") && i < argc - 2){
			paths[SPRITES] = argv[i+1];
			i++;
		}else if(!strcmp(argv[i], "-o") && i < argc - 2){
			paths[SHOOTERS] = argv[i+1];
			i++;
		}else if(!strcmp(argv[i], "-g") && i < argc - 2){
			paths[GENERATORS] = argv[i+1];
			i++;
		}else if(!strcmp(argv[i], "-l") && i < argc - 2){
			paths[LIST] = argv[i+1];
			i++;
		}else{
			if(i == argc-1){
				break;
			}
			error("ERROR: Invalid command line option \"%s\".\n", argv[i]);
		}
	}

	if(argc < 2){
		printf("Enter a ROM file name, or drag and drop the ROM here: ");
		char ROM_name[FILENAME_MAX];
		if(fgets(ROM_name, FILENAME_MAX, stdin)){
			int length = strlen(ROM_name)-1;
			ROM_name[length] = 0;
			if((ROM_name[0] == '"' && ROM_name[length - 1] == '"') ||
			   (ROM_name[0] == '\'' && ROM_name[length - 1] == '\'')){
				ROM_name[length -1] = 0;
				for(int i = 0; ROM_name[i]; i++){
					ROM_name[i] = ROM_name[i+1]; //no buffer overflow there are two null chars.
				}
			}
		}
		rom.open(ROM_name);
	}else{
		rom.open(argv[argc-1]);
	}
	
	populate_sprite_list(paths, sprite_list, data.full_table, (char *)read_all(paths[LIST], true), debug_flag);
		
	clean_hack(rom);
		
	create_shared_patch(paths[ROUTINES], rom);
	
	patch_sprites(sprite_list, rom, debug_flag);
		
	write_all(versionflag, "asm/_versionflag.bin", 4);	
	write_all((unsigned char*)data.default_table, "asm/_DefaultTables.bin", 0xF00);
	write_all((unsigned char*)data.level_table_t1, "asm/_PerLevelT1.bin", 0x8000);	
	write_all((unsigned char*)data.level_table_t2, "asm/_PerLevelT2.bin", 0x8000);	
	write_all((unsigned char*)data.level_table_t3, "asm/_PerLevelT3.bin", 0x8000);	
	write_all((unsigned char*)data.level_table_t4, "asm/_PerLevelT4.bin", 0x8000);
		
	patch("asm/main.asm", rom, "asm/main.asm");	
	
	if(!keep_temp){	
		remove("_versionflag.bin");
		remove("_DefaultTables.bin");
		remove("_PerLevelT1.bin");
		remove("_PerLevelT2.bin");
		remove("_PerLevelT3.bin");
		remove("_PerLevelT4.bin");
	}
	
	rom.close();
	asar_close();
	printf("\nAll sprites applied successfully!\n");
	return 0;
}