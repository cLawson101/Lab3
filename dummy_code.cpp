// program named mainreturn.c
#include <iostream>

#include <algorithm>
#include <cstring>
#include <stdint.h>

// for mmap:
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>


#include <unistd.h>
#include <stdio.h>

// for signal catching
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

// for switch to user

using namespace std;

#include "switch.h"
#include "my_elf.h"

#include <stdlib.h>
#include <stdio.h>
#include <elf.h>
#include <assert.h>
#include <string.h>

#define PAGE_SIZE	4096
#define ALIGN		(PAGE_SIZE - 1)
#define ROUND_PG(x)	(((x) + (ALIGN)) & ~(ALIGN))
#define TRUNC_PG(x)	((x) & ~(ALIGN))
#define PFLAGS(x)	((((x) & 0x1) ? PROT_READ : 0) | \
                     (((x) & 0x2) ? PROT_WRITE : 0) | \
                     (((x) & 0x4) ? PROT_EXEC : 0))

#define ELF_DEBUG 0
#define STACK_DEBUG 1

int exacutable = -1;

uint32_t len(const char* str){
    uint32_t l = 0;
    while(str[l]){
        l++;
    }
    l++;
    return l;
}

void segfault_func(int sig, siginfo_t *si, void *arg){
    printf("CAUGHT A SIGNAL %d\n", sig);
    printf("vaddr at: %08lx\n", (uint64_t)si->si_addr);

    printf("Exacutable %d\n", exacutable);

    exit(-1);
    return;
}

void stack_check(void* top_of_stack, uint64_t argc, char** argv) {
	printf("----- stack check -----\n");

	assert(((uint64_t)top_of_stack) % 8 == 0);
	printf("top of stack is 8-byte aligned\n");

	uint64_t* stack = (uint64_t*)top_of_stack;
	uint64_t actual_argc = *(stack++);
	printf("argc: %lu\n", actual_argc);
	assert(actual_argc == argc);

	for (int i = 0; i < argc; i++) {
		char* argp = (char*)*(stack++);
		assert(strcmp(argp, argv[i]) == 0);
		printf("arg %d: %s\n", i, argp);
	}
	// Argument list ends with null pointer
	assert(*(stack++) == 0);

	int envp_count = 0;
	while (*(stack++) != 0)
		envp_count++;

	printf("env count: %d\n", envp_count);

	Elf64_auxv_t* auxv_start = (Elf64_auxv_t*)stack;
	Elf64_auxv_t* auxv_null = auxv_start;
	while (auxv_null->a_type != AT_NULL) {
		auxv_null++;
	}
	printf("aux count: %lu\n", auxv_null - auxv_start);
	printf("----- end stack check -----\n");
}

 
// defining main with arguments
int main(int argc, char* argv[], char* envp[])
{
    printf("GIVEN THE FOLLOWING\n");
    printf("You have entered %d arguments:\n", argc);

    printf("You have this for envp: %lx\n", (uint64_t) envp);

    uint64_t user_argc = argc - 1;
 
    for (int i = 0; i < argc; i++) {
        printf("%s\n", argv[i]);
    }

    printf("About to open: %s\n", argv[1]);
    int fd = open(argv[1], O_RDONLY);

    if (fd == -1){
        perror("open error");
        exit(255);
    }

    exacutable = fd;
    printf("FD is: %d\n", fd);

    ElfHeader* header = (ElfHeader*) malloc(sizeof(ElfHeader));
    ProgramHeader* program_header = (ProgramHeader*) malloc(sizeof(ProgramHeader));
    SectionHeader* section_header = (SectionHeader*) malloc(sizeof(SectionHeader));
    int flags = MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS;

    int ret = read(fd, header, sizeof(ElfHeader));
    if(ret == -1){
        perror("Read error");
        exit(255);
    }

    if(ELF_DEBUG){
        printf("Size of header = %ld\n", sizeof(ElfHeader));
        printf("Size of prog header = %ld\n", sizeof(ProgramHeader));
        printf("        prog header = %d\n", header->phentsize);
        printf("Size of sect header = %ld\n", sizeof(SectionHeader));
        printf("        sect header = %d\n", header->shentsize);
        printf("header->magic0: %x\n", header->magic0);
        printf("header->magic1: %x\n", header->magic1);
        printf("header->magic2: %x\n", header->magic2);
        printf("header->magic3: %x\n", header->magic3);
        printf("header->class: %x\n", header->ei_class);

        printf("header->entry: %8lx\n", header->e_entry);

        printf("header->phnum: %3d\n", header->phnum);
        printf("header->e_phoff: %8lx\n", header->e_phoff);
        printf("header->shnum: %3d\n", header->shnum);
        printf("header->e_shoff: %8lx\n", header->e_shoff);
        printf("\nSEPARATOR\n");
    }

    ret = lseek(fd, header->e_phoff, SEEK_SET);

    if(ret == -1){
        perror("lseek error");
        exit(255);
    }

    // Make sure we can hold the entire image;

    uint64_t minva = (uint64_t) (-1);
    uint64_t maxva = 0;

    for(uint32_t i = 0; i < header->phnum; i++){
        ret = read(fd, program_header, sizeof(ProgramHeader));

        if(ret == -1){
            perror("Read error");
            exit(255);
        }

        if(program_header->p_type != 1)
            continue;
        
        if(ELF_DEBUG){
            printf("PROGRAM HEADER: %d\n", i);
            printf("type: %x\n", program_header->p_type);
            printf("flags: %x\n", program_header->p_flags);
            printf("offset: %lx\n", program_header->p_offset);
            printf("vaddr: %lx\n", program_header->p_vaddr);
            printf("paddr: %lx\n", program_header->p_paddr);
            printf("filesz: %lx\n", program_header->p_filesz);
            printf("memsz: %lx\n", program_header->p_memsz);
            printf("align: %lx\n", program_header->p_align);

            printf("Allocating address: %lx\n", program_header->p_vaddr);
            printf("Allocating memsize: %lx\n\n", program_header->p_memsz);
        }

        if((uint64_t)program_header->p_vaddr < minva){
            // printf("CHANGING MINVA\n");
            minva = (uint64_t)program_header->p_vaddr;
        }
        if((uint64_t)program_header->p_vaddr + (uint64_t)program_header->p_memsz > maxva){
            // printf("CHANGING MAXVA\n");
            maxva = (uint64_t)program_header->p_vaddr + (uint64_t)program_header->p_memsz;
        }
    }

    minva = TRUNC_PG(minva);
    maxva = ROUND_PG(maxva);

    if(ELF_DEBUG){
        printf("minva: %08lx\n", minva);
        printf("maxva: %08lx\n", maxva);
        printf("Allocating: %lx\n", (ssize_t)(maxva - minva));
    }


    char* base = (char*) mmap((void*)minva, maxva - minva, PROT_NONE, flags, -1, 0);

    if(base == (void*)-1){
        perror("Cannot hold full image");
        exit(255);
    }

    munmap(base, maxva - minva);

    if(ELF_DEBUG) {
        printf("CAN HOLD FULL IMAGE\n");
    }

    for(uint32_t i = 0; i < header->phnum; i++){

        uint64_t off, start;
        ssize_t sz;

        ret = lseek(fd, header->e_phoff + i * sizeof(ProgramHeader), SEEK_SET);
        
        // printf("\nGoing to offset: %08lx\n", (uint64_t)(header->e_phoff + i * sizeof(ProgramHeader)));
        
        if(ret == -1){
            perror("Lseek error");
            exit(255);
        }

        ret = read(fd, program_header, sizeof(ProgramHeader));

        if(ret == -1){
            perror("Read error");
            exit(255);
        }


        if(program_header->p_type != 1){
            continue;
        }

        if(ELF_DEBUG){
            printf("PROGRAM HEADER: %d\n", i);
            printf("type: %x\n", program_header->p_type);
            printf("flags: %x\n", program_header->p_flags);
            printf("offset: %lx\n", program_header->p_offset);
            printf("vaddr: %lx\n", program_header->p_vaddr);
            printf("paddr: %lx\n", program_header->p_paddr);
            printf("filesz: %lx\n", program_header->p_filesz);
            printf("memsz: %lx\n", program_header->p_memsz);
            printf("align: %lx\n", program_header->p_align);

            printf("Loading Current program header\n");
        }

        off = program_header->p_vaddr & ALIGN;
        start = 0;
        start += TRUNC_PG(program_header->p_vaddr);
        sz = ROUND_PG(program_header->p_memsz + off);

        if(ELF_DEBUG){
            printf("\n");
            printf("off: %08lx\n", (uint64_t)off);
            printf("start: %08lx\n", (uint64_t)start);
            printf("sz: %08lx\n", (uint64_t)sz);

            printf("About to mmap\n");  
        }
        
        char* p = (char*)mmap((void*)start, sz, PROT_WRITE, flags, -1, 0u);

        if( p == (void*) -1){
            perror("mmap error");
            exit(255);
        }

        if(ELF_DEBUG){
            printf("Finished mmap\n");
            printf("p: %08lx\n", (uint64_t)p);
        }

        int lseek_ret = lseek(fd, program_header->p_offset, SEEK_SET);

        if( lseek_ret < 0){
            perror("lseek error");
            exit(255);
        }

        int read_ret = read(fd, p + off, program_header->p_filesz);

        if( read_ret != (ssize_t)program_header->p_filesz){
            perror("read error");
            exit(255);
        }
        
        mprotect(p, sz, PFLAGS(program_header->p_flags));

        if(ELF_DEBUG){
            printf("\nSEPARATOR\n");
        }
    }

    uint64_t* user_esp_addr = (uint64_t*) mmap(NULL, 1 << 30, PROT_WRITE | PROT_READ, MAP_SHARED | MAP_ANON, -1, 0u);
    // uint64_t* user_esp_addr = (uint64_t*) mmap((void*)0x6ff786acc000, 1 << 30, PROT_WRITE | PROT_READ, MAP_ANON, -1, 0u);

    uint64_t user_esp_orig = reinterpret_cast<std::uintptr_t>(user_esp_addr);
    user_esp_addr = (uint64_t*)(((uint64_t)user_esp_addr) + (1 << 12));

    if(STACK_DEBUG){
        printf("User esp set at: %0lx\n", user_esp_orig);
        printf("User esp set at: %0lx\n", (uint64_t) user_esp_addr);
    }

    // Insert the envp arguments
    uint64_t num_envp = 0;
    while(envp[num_envp]){
        printf("data[%ld]: %s\n", num_envp, (char*)envp[num_envp]);
        num_envp++;
    }

    // Figure out how many Aux vectors we have
    char** p = &envp[num_envp + 1];

    uint64_t num_aux = 0;
    Elf64_auxv_t* av;

    av = (Elf64_auxv_t*) p;

    while(av[num_aux].a_type != AT_NULL){
        printf("av[%02ld] av value: %02ld\n", num_aux, av[num_aux].a_type);
        num_aux++;
    }

    user_esp_addr[0] = user_argc;

    // Will:
    // Section taken up by argv pointers
    // Section taken up by envp pointers
    // Section taken up by auxv pointers
    uint64_t data_start = (uint64_t)&user_esp_addr[1] + (user_argc+1) * 8;
    data_start += (num_envp + 1) * 8;
    data_start += (num_aux + 1) * 16;

    uint32_t total_offset = 0;

    for(int i = 0; i < user_argc; i++){
        user_esp_addr[1+i] = i + 1;
    }

    user_esp_addr[1 + user_argc] = 0;

    for(int i = 0; i < num_envp; i++){
        user_esp_addr[1 + user_argc + 1 + i] = 0xDEADBE00 + i;
    }

    for(int i = 0; i < num_aux; i++){
        user_esp_addr[1 + user_argc + 1 + num_envp + 1 + i] = 0xBEEFDE00 + i;
    }

    printf("%ld\n", user_argc + 1);
    printf("%ld\n", num_envp + 1);
    printf("%ld\n", num_aux + 1);
    printf("%ld\n", 1 + user_argc + 1 + num_envp + 1 + num_aux + 1);
    user_esp_addr[1 + user_argc + 1 + num_envp + 1 + num_aux + 1] = 0;
    printf("Passed\n");

    // Start Filling in values

    // First start with data values:
    user_esp_addr[1] = data_start;

    for(int i = 2; i < user_argc + 1; i++){
        printf("Looking at %s\n", argv[i-1]);
        user_esp_addr[i] = user_esp_addr[i - 1] + len(argv[i-1]);
    }

    // Next insert envp pointers

    printf("last item is: %08lx\n", user_esp_addr[user_argc]);
    printf("Last element is: %s\n", argv[user_argc]);
    printf("Last element len is: %d\n", len(argv[user_argc]));
    uint64_t end_arg_data = user_esp_addr[user_argc] + len(argv[user_argc]);
    uint64_t arg_data_pad = 8 - (end_arg_data % 8);
    printf("Ending Data %08lx\n", end_arg_data);
    printf("Padding %ld\n", arg_data_pad);

    uint64_t start_env_data = end_arg_data + arg_data_pad;

    printf("Starting env_data: %08lx\n", start_env_data);

    user_esp_addr[user_argc + 2] = start_env_data;

    for(int i = 1; i < num_envp; i++){
        printf("Looking at %s\n", envp[i-1]);
        printf("len is: %d\n", len(envp[i-1]));
        user_esp_addr[user_argc + 2 + i] = user_esp_addr[user_argc + 2 + i - 1] + len(envp[i-1]);
    }

    // Now to actually fill in the data that we have.

    // Start with the argv data

    for(int arg = 0; arg < user_argc; arg++){
        char* data_loc = (char*) user_esp_addr[1+arg];
        char* data = argv[1+arg];

        printf("Location to copy is: %8lx\n", (uint64_t)data_loc);
        printf("Source to copy is  : %8lx\n", (uint64_t)data);
        printf("Data is            : %s\n", data);
        printf("Len(data) is       : %d\n", len(data));
        memcpy(data_loc, data, len(data));
    }

    printf("FINISHED ARGV DATA\n\n");

    // Now the envp data

    for(int cur_env = 0; cur_env < num_envp; cur_env++){
        char* data_loc = (char*) user_esp_addr[1 + (user_argc + 1) + cur_env];
        char* data = envp[cur_env];

        printf("Location to copy is: %8lx\n", (uint64_t)data_loc);
        printf("Source to copy is  : %8lx\n", (uint64_t)data);
        printf("Data is            : %s\n", data);
        printf("Len(data) is       : %ld\n", sizeof(Elf64_auxv_t));
        memcpy(data_loc, data, len(data));
    }

    printf("FINISHED ENVP DATA\n\n");

    // Lastly the aux data
    for(int cur_aux = 0; cur_aux < num_aux; cur_aux++){

        uint64_t start_aux_addr = (uint64_t)&user_esp_addr[1 + (user_argc + 1) + (num_envp + 1)];

        char* data_loc = (char*) (start_aux_addr + cur_aux * sizeof(Elf64_auxv_t));
        char* data = (char*)&av[cur_aux];

        printf("Location to copy is: %8lx\n", (uint64_t)data_loc);
        printf("Source to copy is  : %8lx\n", (uint64_t)data);
        printf("Data is            : %ld\n", av[cur_aux].a_type);
        memcpy(data_loc, data, sizeof(Elf64_auxv_t));
    }

    printf("FINISHED AUX DATA\n\n");

    
    printf("Last env pointer: %08x\n", (uint64_t)&user_esp_addr[1 + user_argc + 1 + num_envp + 1]);

    // CHECK THE VALIDITY OF THE STACK
    // printf("ARGC:\n");
    // printf("user_esp_addr[%2d] (%8lx) - %08lx\n", 0, (uint64_t)(user_esp_addr), (uint64_t)user_esp_addr[0]);
    
    // printf("ARGV:\n");
    // for(int i = 0; i < user_argc; i++){
    //     printf("user_esp_addr[%2d] - %s\n", i + 1, (char*)user_esp_addr[i+1]);
    // }

    // printf("ARGV ENDS:\n");
    // printf("user_esp_addr[%2d] - %d\n", 1 + user_argc, user_esp_addr[1 + user_argc]);
    
    // printf("ENVP:\n");
    // for(int i = 0; i < num_envp; i++){
    //     printf("user_esp_addr[%2d] - %s\n", 1 + user_argc + 1 + i, (char*)user_esp_addr[1 + user_argc + 1 + i]);
    // }
    // printf("ENVP ENDS:\n");
    // printf("user_esp_addr[%2d] - %d\n", 1 + user_argc + 1 + num_envp, user_esp_addr[1 + user_argc + 1 + num_envp]);

    // printf("AUX:\n");

    // for(int i = 0; i < num_aux * 2; i++){
    //     printf("user_esp_addr[%2d] - %x\n", 1 + user_argc + 1 + num_envp + 1 + i, user_esp_addr[1 + user_argc + 1 + num_envp + 1 + i]);
    // }

    // printf("AUX ENDS\n");
    // printf("user_esp_addr[%2d] - %d\n", 1 + user_argc + 1 + num_envp + 1 + num_aux * 2, user_esp_addr[1 + user_argc + 1 + num_envp + 1 + num_aux * 2]);

    // Now need to do some modification to the aux vectors in accordance with
    // loader.c from the github repo

    printf("AT_PHDR is: %d\n", AT_PHDR);
    printf("AT_PHENT is: %d\n", AT_PHENT);
    printf("AT_PHNUM is: %d\n", AT_PHNUM);
    printf("AT_BASE is: %d\n", AT_BASE);
    printf("AT_ENTRY is: %d\n", AT_ENTRY);
    printf("AT_EXECFN is: %d\n", AT_EXECFN);

    #define AVSET(t, v, expr) case (t): (v)->a_un.a_val = (expr); break
    for(int i = 0; i < num_aux; i++){
        uint64_t start_aux_addr = (uint64_t)&user_esp_addr[1 + (user_argc + 1) + (num_envp + 1)];

        Elf64_auxv_t* cur = (Elf64_auxv_t*)(start_aux_addr + sizeof(Elf64_auxv_t) * i);

        switch (cur->a_type){
            AVSET(AT_PHDR, cur, minva + header->e_phoff);
            AVSET(AT_PHENT, cur, header->phentsize);
            AVSET(AT_PHNUM, cur, header->phnum);
            AVSET(AT_BASE, cur, av->a_un.a_val);
            AVSET(AT_ENTRY, cur, header->e_entry);
            AVSET(AT_EXECFN, cur, (unsigned long)user_esp_addr[1]);
        }
    }

    // printf("STACK SEPARATOR\n\n");

    // for(int i = 0; i < 86; i++){
    //     printf("user_esp_addr[%2d] (%8lx) - %8lx\n", i, (uint64_t)(user_esp_addr+i), (uint64_t)user_esp_addr[i]);
    // }

    // return 0;


    // GETTING THE ARG TRAP SET
    // Taken from:
    // https://cplusplus.com/forum/unices/16430/

    struct sigaction act;
    act.sa_sigaction = segfault_func;
    sigemptyset(&act.sa_mask);
    act.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &act, 0);

    printf("Entry is: %08lx\n", header->e_entry);
    printf("User stack: %08lx\n", (uint64_t)(user_esp_addr));

    uint64_t* cur_addr = (uint64_t*)(user_esp_addr);

    stack_check((void*)user_esp_addr, user_argc, &argv[1]);

    printf("ABOUT TO SWITCH TO USER\n");
    switchToUser(header->e_entry, (uint64_t)(user_esp_addr));
    printf("SHOULD NOT PRINT\n");

    uint8_t* start_read = (uint8_t*) 0x404510;
    uint64_t num_lines = 5;

    for(int i = 0; i < num_lines; i++){
        printf("%08lx - %02x\n", (uint64_t)(start_read + i), *(start_read + i));
    }

    printf("\n");

    start_read = (uint8_t*) 0x404648;
    num_lines = 5;

    for(int i = 0; i < num_lines; i++){
        printf("%08lx - %02x\n", (uint64_t)(start_read + i), *(start_read + i));
    }

    printf("\n");

    start_read = (uint8_t*) 0x5D6E09;
    num_lines = 8;

    for(int i = 0; i < num_lines; i++){
        printf("%08lx - %02x\n", (uint64_t)(start_read + i), *(start_read + i));
    }
    
    return 0;
}