extern char __stack_top[];
extern char __bss[];
extern char __bss_end[];

void main(void);

struct sbiret {
    long error;
    long value;
};

struct sbiret sbi_call(long arg0, long arg1, long arg2, long arg3,
                       long arg4, long arg5, long fid, long eid) {
    register long a0 __asm__("a0") = arg0;
    register long a1 __asm__("a1") = arg1;
    register long a2 __asm__("a2") = arg2;
    register long a3 __asm__("a3") = arg3;
    register long a4 __asm__("a4") = arg4;
    register long a5 __asm__("a5") = arg5;
    register long a6 __asm__("a6") = fid;
    register long a7 __asm__("a7") = eid;
    __asm__ __volatile__("ecall"
        : "+r"(a0), "+r"(a1)
        : "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a6), "r"(a7)
        : "memory");
    return (struct sbiret){a0, a1};
}

void putchar(char ch) {
    sbi_call(ch, 0, 0, 0, 0, 0, 0, 1);
}

int getchar_nonblock(void) {
    struct sbiret ret = sbi_call(0, 0, 0, 0, 0, 0, 0, 2);
    return ret.error;
}

int getchar(void) {
    int ch;
    while ((ch = getchar_nonblock()) < 0);
    return ch;
}

long read_time(void) {
    long value;
    __asm__ __volatile__("rdtime %0" : "=r"(value));
    return value;
}

void msleep(int msec) {
    long start = read_time();
    while (read_time() - start < msec * 10000L);
}

void exit(long status) {
    int code = status ? 0x3333 | (status << 16) : 0x5555;
    *(volatile int *)0x100000 = code;
    for (;;) {
        __asm__ __volatile__("wfi");
    }
}

void kernel_main(void) {
    for (char *p = __bss; p < __bss_end; p++) {
        *p = 0;
    }
    main();
    exit(0);
}

__attribute__((section(".text.boot"), naked))
void boot(void) {
    __asm__ __volatile__(
        "mv sp, %[stack_top]\n"
        "j kernel_main\n"
        :
        : [stack_top] "r" (__stack_top)
    );
}
