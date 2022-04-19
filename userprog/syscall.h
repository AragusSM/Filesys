#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

extern struct lock f_lock;

void syscall_init (void);
//So exception.c can also use this 
//function
void exit(int status);

#endif /* userprog/syscall.h */
