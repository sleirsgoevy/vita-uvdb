#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/signal.h> //for signal constants; these seem to match gdb's
#include <stdarg.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <psp2/net/net_syscalls.h>
#include <psp2/kernel/threadmgr/msgpipe.h>
#include <kubridge.h>
#include "uvdb.h"

//we prefer to use raw syscalls to avoid issues with signal safety
void _sceKernelExitProcessForUser(int);
int _sceKernelSendMsgPipeVector(SceUID, const SceKernelAddrPair*, unsigned int, uint32_t* rest);
int _sceKernelReceiveMsgPipeVector(SceUID, const SceKernelAddrPair*, unsigned int, uint32_t* rest);
extern char __executable_start[];
extern char __init_array_start[];

static int uvdb_lock_state;

static void uvdb_lock(void)
{
    for(;;)
    {
        int old_value = 0;
        if(__atomic_compare_exchange_n(&uvdb_lock_state, &old_value, 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            return;
    }
}

static void uvdb_unlock(void)
{
    __atomic_store_n(&uvdb_lock_state, 0, __ATOMIC_SEQ_CST);
}

static int uvdb_socket = -1;
static SceUID uvdb_pipe = -1;

struct buffer
{
    SceUID memblock_uid;
    char* buf;
    size_t size;
    size_t cap;
    size_t packet_start;
};

static void buffer_popleft(struct buffer* buf, size_t cnt)
{
    memmove(buf->buf, buf->buf+cnt, buf->size-cnt);
    buf->size -= cnt;
}

static size_t buffer_getspace(struct buffer* buf, char** pos)
{
    if(buf->size == buf->cap)
    {
        size_t cap2 = buf->cap * 2;
        if(!cap2)
            cap2 = 4096;
        SceUID memblock2 = sceKernelAllocMemBlock("gdb socket buffer", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, cap2, NULL);
        if(memblock2 < 0)
        {
            *pos = NULL;
            return 0;
        }
        void* base;
        sceKernelGetMemBlockBase(memblock2, &base);
        memcpy(base, buf->buf, buf->size);
        sceKernelFreeMemBlock(buf->memblock_uid);
        buf->memblock_uid = memblock2;
        buf->buf = base;
        buf->cap = cap2;
    }
    *pos = buf->buf + buf->size;
    return buf->cap - buf->size;
}

static size_t buffer_poll(struct buffer* buf, char** pos)
{
    size_t chk_size = buffer_getspace(buf, pos);
    uint32_t args[6] = {uvdb_socket, (uint32_t)*pos, chk_size, 0, 0, 0};
    ssize_t ans = sceNetSyscallRecvfrom((void*)args);
    if(ans < 0)
        ans = 0;
    buf->size += ans;
    return ans;
}

static void buffer_write(struct buffer* buf, const char* data, size_t sz)
{
    while(sz)
    {
        char* pos;
        size_t chk = buffer_getspace(buf, &pos);
        if(chk > sz)
            chk = sz;
        memcpy(pos, data, chk);
        data += chk;
        buf->size += chk;
        sz -= chk;
    }
}

static void buffer_start_packet(struct buffer* buf)
{
    buffer_write(buf, "$", 1);
    buf->packet_start = buf->size;
}

static char int2hex(int value)
{
    if(value < 10)
        return value + '0';
    return value - 10 + 'a';
}

static void buffer_end_packet(struct buffer* buf)
{
    uint8_t cksum = 0;
    for(size_t i = buf->packet_start; i < buf->size; i++)
        cksum += (uint8_t)buf->buf[i];
    uint8_t footer[3] = {'#', int2hex(cksum>>4), int2hex(cksum&15)};
    buffer_write(buf, footer, 3);
}

static void buffer_flush(struct buffer* buf)
{
    size_t pos = 0;
    while(pos < buf->size)
    {
        uint32_t args[6] = {uvdb_socket, (uint32_t)(buf->buf+pos), buf->size-pos, 0, 0, 0};
        ssize_t chk = sceNetSyscallSendto((void*)args);
        if(chk < 0)
            chk = 0;
        pos += chk;
    }
    buf->size = 0;
}

static struct buffer in_buf, out_buf;

#define POLL() while(cur == end) { size_t sz = buffer_poll(&in_buf, &cur); end = cur + sz; }

static size_t recv_packet(char** data)
{
    char* cur = in_buf.buf;
    char* end = cur + in_buf.size;
retry:;
    char c = 0;
    while(c != '$')
    {
        POLL();
        c = *cur++;
    }
    size_t start_packet = cur - in_buf.buf;
    while(c != '#')
    {
        POLL();
        c = *cur++;
    }
    size_t end_packet = cur - in_buf.buf - 1;
    uint8_t cksum = 0;
    for(size_t i = start_packet; i < end_packet; i++)
        cksum += (uint8_t)in_buf.buf[i];
    POLL();
    char c1 = *cur++;
    POLL();
    char c2 = *cur++;
    if(c1 != int2hex(cksum>>4) || c2 != int2hex(cksum&15))
        goto retry;
    buffer_write(&out_buf, "+", 1);
    //we better not do this here, so that this + and the reply can be merged into a single packet
    //UPD: it seems that this way the communication is a bit more reliable, so let's keep it
    buffer_flush(&out_buf); 
    *data = in_buf.buf + start_packet;
    in_buf.buf[end_packet] = 0;
    return end_packet - start_packet;
}

static void discard_packet(char* data, size_t sz)
{
    buffer_popleft(&in_buf, data - in_buf.buf + sz + 3);
}

static void send_packet(void)
{
    buffer_end_packet(&out_buf);
    buffer_flush(&out_buf);
    char* cur = in_buf.buf;
    char* end = cur + in_buf.size;
    char c = 0;
    while(c != '+')
    {
        POLL();
        c = *cur++;
    }
    buffer_popleft(&in_buf, cur - in_buf.buf);
}

#undef POLL
#define IS(s) sz == sizeof(s) - 1 && !memcmp(pkt, s, sizeof(s) - 1)
#define STARTSWITH(s) sz >= sizeof(s) - 1 && !memcmp(pkt, s, sizeof(s) - 1)
#define STRING(s) s, sizeof(s) - 1

struct stream
{
    uint64_t cur;
    uint64_t start;
    uint64_t end;
};

#define PARSE_HEX(type, name, cond) static type name(char** s)\
{\
    type ans = 0;\
    for(cond)\
    {\
        char c = *(*s)++;\
        if(c >= '0' && c <= '9')\
            ans = 16 * ans + (c - '0');\
        else\
        {\
            c &= -33;\
            if(c >= 'A' && c <= 'F')\
                ans = 16 * ans + 10 + (c - 'A');\
            else\
                return ans;\
        }\
    }\
    return ans;\
}

PARSE_HEX(uint64_t, parse_hex, ;;)
PARSE_HEX(uint8_t, parse_hex_byte, int c = 0; c < 2; c++)

#undef PARSE_HEX

static struct stream parse_stream(char* s)
{
    struct stream ans = {};
    ans.start = parse_hex(&s);
    ans.end = parse_hex(&s);
    return ans;
}

static void stream_write(struct stream* st, char* buf, size_t sz)
{
    if(st->cur < st->start)
    {
        size_t chk = st->start - st->cur;
        if(sz <= chk)
        {
            st->cur += sz;
            return;
        }
    }
    if(st->cur < st->end)
    {
        size_t chk = st->end - st->cur;
        if(sz < chk)
            chk = sz;
        if(chk && st->cur == st->start)
            buffer_write(&out_buf, "m", 1);
        buffer_write(&out_buf, buf, chk);
        st->cur += chk;
    }
}

static void stream_close(struct stream* st)
{
    if(st->cur <= st->start)
        buffer_write(&out_buf, "l", 1);
}

static void write_hex(char* start, size_t sz)
{
    while(sz--)
    {
        uint8_t c = *start++;
        uint8_t q[2] = {int2hex(c>>4), int2hex(c&15)};
        buffer_write(&out_buf, q, 2);
    }
}

static void read_hex(char** p, char* start, size_t sz)
{
    while(sz--)
    {
        if(**p == 'x' && (*p)[1] == 'x')
        {
            p += 2;
            start++;
        }
        else if(!**p || !(*p)[1])
            *start++ = 0;
        else
            *start++ = parse_hex_byte(p);
    }
}

static void skip_hex(char** p, size_t cnt)
{
    *p += strnlen(*p, 2*cnt);
}

static void write_x(size_t sz)
{
    while(sz--)
        buffer_write(&out_buf, "xx", 2);
}

static size_t safe_memcpy(char* dst, const char* src, size_t sz)
{
    size_t ans = 0;
    while(sz)
    {
        size_t chk;
        uint32_t rest[3] = {1, (uint32_t)&chk, 0};
        SceKernelAddrPair q = {(uint32_t)src, sz};
        if(_sceKernelSendMsgPipeVector(uvdb_pipe, &q, 1, rest))
            break;
        ans += chk;
        src += chk;
        sz -= chk;
        while(chk)
        {
            size_t chk2;
            uint32_t rest[3] = {1, (uint32_t)&chk2, 0};
            SceKernelAddrPair q = {(uint32_t)dst, chk};
            if(_sceKernelReceiveMsgPipeVector(uvdb_pipe, &q, 1, rest))
                chk2 = 0;
            dst += chk2;
            chk -= chk2;
        }
    }
    return ans;
}

static void uvdb_main_loop(KuKernelExceptionContext* ctx, int stop_signal)
{
    for(;;)
    {
        char* pkt;
        size_t sz = recv_packet(&pkt);
        buffer_start_packet(&out_buf);
        if(STARTSWITH("qSupported:"))
            buffer_write(&out_buf, STRING("qXfer:features:read+"));
        else if(STARTSWITH("qXfer:features:read:target.xml:"))
        {
            struct stream st = parse_stream(pkt + sizeof("qXfer:features:read:target.xml:") - 1);
            stream_write(&st, STRING("<?xml version=\"1.0\"?>\n<!DOCTYPE target SYSTEM \"gdb-target.dtd\">\n<target>\n<architecture>armv7</architecture>\n<osabi>GNU/Linux</osabi>\n</target>\n"));
            stream_close(&st);
        }
        else if(IS("?"))
        {
            uint8_t pkt[3] = {'T', int2hex(stop_signal>>4), int2hex(stop_signal&15)};
            buffer_write(&out_buf, pkt, 3);
        }
        else if(IS("g"))
        {
            write_hex((void*)ctx, 16*4);
            write_x(25*4);
            write_hex((void*)&ctx->SPSR, 4);
        }
        else if(STARTSWITH("m"))
        {
            char* p = pkt + 1;
            uintptr_t addr = parse_hex(&p);
            size_t size = parse_hex(&p);
            while(size)
            {
                size_t chk = size;
                if(chk > 64)
                    chk = 64;
                char buf[64];
                size_t copy_sz = safe_memcpy(buf, (void*)addr, chk);
                write_hex(buf, copy_sz);
                if(copy_sz < chk)
                    break;
                addr += chk;
                size -= chk;
            }
        }
        else if(STARTSWITH("G"))
        {
            char* p = pkt + 1;
            read_hex(&p, (void*)ctx, 16*4);
            skip_hex(&p, 25*4);
            read_hex(&p, (void*)&ctx->SPSR, 4);
            buffer_write(&out_buf, "OK", 2);
        }
        else if(STARTSWITH("M"))
        {
            char* p = pkt + 1;
            uintptr_t addr = parse_hex(&p);
            size_t size = parse_hex(&p);
            while(size)
            {
                size_t chk = size;
                if(chk > 64)
                    chk = 64;
                char buf[64];
                read_hex(&p, buf, chk);
                char test[64];
                //this is racey, but should work in practice
                size_t safe_size = safe_memcpy(test, (void*)addr, chk);
                kuKernelCpuUnrestrictedMemcpy((void*)addr, buf, safe_size);
                kuKernelFlushCaches((void*)addr, safe_size);
                if(safe_size < chk)
                    break;
                addr += chk;
                size -= chk;
            }
            if(size)
                buffer_write(&out_buf, "E0e", 3);
            else
                buffer_write(&out_buf, "OK", 2);
        }
        else if(IS("k"))
            _sceKernelExitProcessForUser(1);
        else if(IS("c"))
        {
            memcpy(pkt, "?#3f", 4); //next invocation of uvdb_main_loop will parse it and respond with the status
            out_buf.size--; //undo buffer_start_packet
            buffer_flush(&out_buf); //see the comment in recv_packet
            return; //no cleanup, this is intentional
        }
        else if(STARTSWITH("F"))
        {
            char* p = pkt + 1;
            ctx->r0 = parse_hex(&p);
            //see above for explanation what this does
            memcpy(pkt, "?#3f", 4);
            for(size_t i = 1; i < sz; i++)
                pkt[i+3] = 0;
            out_buf.size--;
            buffer_flush(&out_buf);
            return;
        }
        else if(IS("qOffsets"))
        {
            uint8_t packet[33] = "TextSeg=........;DataSeg=........";
            uint32_t value = (uint32_t)__executable_start;
            for(int i = 0; i < 8; i++)
                packet[15-i] = int2hex((value >> (4*i)) & 15);
            value = (uint32_t)__init_array_start;
            for(int i = 0; i < 8; i++)
                packet[32-i] = int2hex((value >> (4*i)) & 15);
            buffer_write(&out_buf, packet, sizeof(packet));
        }
        discard_packet(pkt, sz);
        send_packet();
    }
}

#undef WRITE
#undef STARTSWITH
#undef IS

static __attribute__((naked)) void uvdb_trap_pc(void)
{
    asm volatile("udf #0");
}

static void exception_handler(KuKernelExceptionContext* ctx)
{
    int signal = SIGSEGV;
    if(ctx->exceptionType == KU_KERNEL_EXCEPTION_TYPE_UNDEFINED_INSTRUCTION)
        signal = SIGILL;
    uint32_t pc = ctx->pc;
    if((ctx->SPSR & 32))
    {
        ctx->SPSR &= -33;
        pc |= 1;
    }
    if(pc == (uint32_t)uvdb_trap_pc)
    {
        pc = ctx->r0;
        signal = SIGTRAP;
    }
    if((pc & 1))
    {
        ctx->SPSR |= 32;
        pc &= -2;
    }
    ctx->pc = pc;
    uvdb_lock();
    uvdb_main_loop(ctx, signal);
    uvdb_unlock();
}

int uvdb_remote_syscall(const char* name, int nargs, ...)
{
    KuKernelExceptionContext ctx = {};
    uvdb_lock();
    if(uvdb_socket < 0)
    {
        //someone attempted to do a remote syscall before the first uvdb_enter
        //do a uvdb_enter now to avoid confusing the code
        uvdb_unlock();
        uvdb_enter();
        uvdb_lock();
    }
    char* pkt;
    size_t sz = recv_packet(&pkt);
    discard_packet(pkt, sz);
    buffer_start_packet(&out_buf);
    buffer_write(&out_buf, "F", 1);
    buffer_write(&out_buf, name, strlen(name));
    va_list va;
    va_start(va, nargs);
    for(int i = 0; i < nargs; i++)
    {
        uintptr_t value = va_arg(va, uintptr_t);
        char packet[9] = ",";
        for(int i = 0; i < 8; i++)
            packet[8-i] = int2hex((value >> (4*i)) & 15);
        buffer_write(&out_buf, packet, 9);
    }
    va_end(va);
    send_packet();
    uvdb_main_loop(&ctx, 0);
    uvdb_unlock();
    return ctx.r0;
}

static __attribute__((used)) uint64_t real_uvdb_enter(uintptr_t lr)
{
    uint64_t no_trap = (uint64_t)lr << 32 | lr;
    uint64_t trap = (uint64_t)(uint32_t)uvdb_trap_pc << 32 | lr;
    uvdb_lock();
    if(uvdb_socket >= 0)
    {
        uvdb_unlock();
        return trap;
    }
    if(uvdb_pipe < 0)
    {
        uvdb_pipe = sceKernelCreateMsgPipe("pipe to catch efault", 0x40, 0xc, 4*4096, NULL);
        if(uvdb_pipe < 0)
        {
            uvdb_unlock();
            return no_trap;
        }
    }
    struct KuKernelExceptionHandlerOpt opt = {
        .size = sizeof(opt),
    };
    KuKernelExceptionHandler old;
    if(kuKernelRegisterExceptionHandler(KU_KERNEL_EXCEPTION_TYPE_DATA_ABORT, exception_handler, &old, &opt)
    || kuKernelRegisterExceptionHandler(KU_KERNEL_EXCEPTION_TYPE_PREFETCH_ABORT, exception_handler, &old, &opt)
    || kuKernelRegisterExceptionHandler(KU_KERNEL_EXCEPTION_TYPE_UNDEFINED_INSTRUCTION, exception_handler, &old, &opt))
    {
        uvdb_unlock();
        return no_trap;
    }
    int sock = sceNetSyscallSocket("gdb socket", AF_INET, SOCK_STREAM, 0);
    if(sock < 0)
    {
        uvdb_unlock();
        return no_trap;
    }
    int value = 1;
    uint32_t args[5] = {sock, SOL_SOCKET, SO_REUSEADDR, (uint32_t)&value, sizeof(value)};
    if(sceNetSyscallSetsockopt((void*)&args))
    {
        uvdb_unlock();
        return no_trap;
    }
    args[1] = IPPROTO_TCP;
    args[2] = TCP_NODELAY;
    if(sceNetSyscallSetsockopt((void*)&args))
    {
        uvdb_unlock();
        return no_trap;
    }
    int port = 1234;
    struct sockaddr_in sin = {
        .sin_family = AF_INET,
        .sin_addr = {},
        .sin_port = 0,
    };
    while(port < 65536 && (sin.sin_port = htons(port), sceNetSyscallBind(sock, &sin, sizeof(sin))))
        port++;
    if(port == 65536)
    {
        uvdb_unlock();
        return no_trap;
    }
    if(sceNetSyscallListen(sock, 1) || (uvdb_socket = sceNetSyscallAccept(sock, NULL, NULL)) < 0)
    {
        uvdb_unlock();
        return no_trap;
    }
    uvdb_unlock();
    return trap;
}

__attribute__((naked)) void uvdb_enter(void)
{
    asm volatile(
        "mov r0, lr\n"
        "bl real_uvdb_enter\n"
        "bx r1\n"
    );
}
