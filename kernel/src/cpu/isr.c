// ============================================================
//  cpu/isr.c — Interrupt dispatch + 8259 PIC management
// ============================================================
#include "isr.h"
#include "idt.h"
#include "apic.h"
#include "../lib/kprintf.h"
#include <stddef.h>

static inline void outb(uint16_t p, uint8_t v) {
    __asm__ volatile ("outb %0,%1"::"a"(v),"Nd"(p):"memory"); }
static inline uint8_t inb(uint16_t p) {
    uint8_t v; __asm__ volatile ("inb %1,%0":"=a"(v):"Nd"(p):"memory"); return v; }
static inline void io_wait(void) { outb(0x80,0); }

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1
#define PIC_EOI   0x20

static void pic_remap(void) {
    uint8_t m1=inb(PIC1_DATA),m2=inb(PIC2_DATA);
    outb(PIC1_CMD,0x11);io_wait(); outb(PIC2_CMD,0x11);io_wait();
    outb(PIC1_DATA,IRQ_BASE);io_wait(); outb(PIC2_DATA,IRQ_BASE+8);io_wait();
    outb(PIC1_DATA,0x04);io_wait(); outb(PIC2_DATA,0x02);io_wait();
    outb(PIC1_DATA,0x01);io_wait(); outb(PIC2_DATA,0x01);io_wait();
    outb(PIC1_DATA,m1); outb(PIC2_DATA,m2);
}

void pic_disable(void) {
    // Mask all interrupts on both PICs (APIC takes over)
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

void pic_send_eoi(uint8_t irq) {
    if (irq >= 8) outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}
void pic_mask_irq(uint8_t irq) {
    uint16_t p=(irq<8)?PIC1_DATA:PIC2_DATA;
    outb(p, inb(p)|(1<<(irq&7)));
}
void pic_unmask_irq(uint8_t irq) {
    uint16_t p=(irq<8)?PIC1_DATA:PIC2_DATA;
    outb(p, inb(p)&~(1<<(irq&7)));
}

static irq_handler_t irq_handlers[256 - IRQ_BASE];
void irq_register_handler(uint8_t irq, irq_handler_t h) {
    if (irq < (256 - IRQ_BASE)) irq_handlers[irq] = h;
}

static const char *const exc_names[32] = {
    "Divide Error","Debug","NMI","Breakpoint","Overflow","Bound Range",
    "Invalid Opcode","Device Not Available","Double Fault",
    "Coprocessor Segment Overrun","Invalid TSS","Segment Not Present",
    "Stack Fault","General Protection","Page Fault","Reserved",
    "x87 FP Exception","Alignment Check","Machine Check","SIMD FP",
    "Virtualisation","Control Protection","Reserved","Reserved","Reserved",
    "Reserved","Reserved","Reserved","Hypervisor Injection",
    "VMM Communication","Security Exception","Reserved"
};

void isr_dispatch(registers_t *r) {
    uint64_t v = r->vector;

    if (v < 32) {
        // CPU exception
        if (v == 3) {
            kprintf("\n[BREAKPOINT] RIP=0x%016llx\n",(unsigned long long)r->rip);
            apic_eoi();
            return;
        }
        kpanic(
            "\n*** Quanta Kernel Panic ***\n"
            "Exception #%llu: %s\n"
            "Error Code : 0x%016llx\n"
            "RIP=0x%016llx  CS=0x%04llx  RFLAGS=0x%016llx\n"
            "RSP=0x%016llx  SS=0x%04llx\n"
            "RAX=0x%016llx  RBX=0x%016llx  RCX=0x%016llx  RDX=0x%016llx\n"
            "RSI=0x%016llx  RDI=0x%016llx  RBP=0x%016llx\n"
            "R8 =0x%016llx  R9 =0x%016llx  R10=0x%016llx  R11=0x%016llx\n"
            "R12=0x%016llx  R13=0x%016llx  R14=0x%016llx  R15=0x%016llx\n",
            (unsigned long long)v, exc_names[v],
            (unsigned long long)r->error_code,
            (unsigned long long)r->rip,(unsigned long long)r->cs,
            (unsigned long long)r->rflags,
            (unsigned long long)r->rsp,(unsigned long long)r->ss,
            (unsigned long long)r->rax,(unsigned long long)r->rbx,
            (unsigned long long)r->rcx,(unsigned long long)r->rdx,
            (unsigned long long)r->rsi,(unsigned long long)r->rdi,
            (unsigned long long)r->rbp,
            (unsigned long long)r->r8,(unsigned long long)r->r9,
            (unsigned long long)r->r10,(unsigned long long)r->r11,
            (unsigned long long)r->r12,(unsigned long long)r->r13,
            (unsigned long long)r->r14,(unsigned long long)r->r15);
    } else {
        uint8_t irq = (uint8_t)(v - IRQ_BASE);
        if (irq_handlers[irq]) {
            irq_handlers[irq](r);
        }
        apic_eoi();
    }
}

// ── Stub declarations ──────────────────────────────────────────────────────
#define S(n) extern void isr_stub_##n(void);
S(0)S(1)S(2)S(3)S(4)S(5)S(6)S(7)S(8)S(9)S(10)S(11)S(12)S(13)S(14)S(15)
S(16)S(17)S(18)S(19)S(20)S(21)S(22)S(23)S(24)S(25)S(26)S(27)S(28)S(29)S(30)S(31)
S(32)S(33)S(34)S(35)S(36)S(37)S(38)S(39)S(40)S(41)S(42)S(43)S(44)S(45)S(46)S(47)
S(48)S(49)S(50)S(51)S(52)S(53)S(54)S(55)S(56)S(57)S(58)S(59)S(60)S(61)S(62)S(63)
S(64)S(65)S(66)S(67)S(68)S(69)S(70)S(71)S(72)S(73)S(74)S(75)S(76)S(77)S(78)S(79)
S(80)S(81)S(82)S(83)S(84)S(85)S(86)S(87)S(88)S(89)S(90)S(91)S(92)S(93)S(94)S(95)
S(96)S(97)S(98)S(99)S(100)S(101)S(102)S(103)S(104)S(105)S(106)S(107)S(108)S(109)S(110)S(111)
S(112)S(113)S(114)S(115)S(116)S(117)S(118)S(119)S(120)S(121)S(122)S(123)S(124)S(125)S(126)S(127)
S(128)S(129)S(130)S(131)S(132)S(133)S(134)S(135)S(136)S(137)S(138)S(139)S(140)S(141)S(142)S(143)
S(144)S(145)S(146)S(147)S(148)S(149)S(150)S(151)S(152)S(153)S(154)S(155)S(156)S(157)S(158)S(159)
S(160)S(161)S(162)S(163)S(164)S(165)S(166)S(167)S(168)S(169)S(170)S(171)S(172)S(173)S(174)S(175)
S(176)S(177)S(178)S(179)S(180)S(181)S(182)S(183)S(184)S(185)S(186)S(187)S(188)S(189)S(190)S(191)
S(192)S(193)S(194)S(195)S(196)S(197)S(198)S(199)S(200)S(201)S(202)S(203)S(204)S(205)S(206)S(207)
S(208)S(209)S(210)S(211)S(212)S(213)S(214)S(215)S(216)S(217)S(218)S(219)S(220)S(221)S(222)S(223)
S(224)S(225)S(226)S(227)S(228)S(229)S(230)S(231)S(232)S(233)S(234)S(235)S(236)S(237)S(238)S(239)
S(240)S(241)S(242)S(243)S(244)S(245)S(246)S(247)S(248)S(249)S(250)S(251)S(252)S(253)S(254)S(255)
#undef S

typedef void(*stub_fn)(void);
static const stub_fn stub_table[256] = {
    isr_stub_0,isr_stub_1,isr_stub_2,isr_stub_3,isr_stub_4,isr_stub_5,isr_stub_6,isr_stub_7,
    isr_stub_8,isr_stub_9,isr_stub_10,isr_stub_11,isr_stub_12,isr_stub_13,isr_stub_14,isr_stub_15,
    isr_stub_16,isr_stub_17,isr_stub_18,isr_stub_19,isr_stub_20,isr_stub_21,isr_stub_22,isr_stub_23,
    isr_stub_24,isr_stub_25,isr_stub_26,isr_stub_27,isr_stub_28,isr_stub_29,isr_stub_30,isr_stub_31,
    isr_stub_32,isr_stub_33,isr_stub_34,isr_stub_35,isr_stub_36,isr_stub_37,isr_stub_38,isr_stub_39,
    isr_stub_40,isr_stub_41,isr_stub_42,isr_stub_43,isr_stub_44,isr_stub_45,isr_stub_46,isr_stub_47,
    isr_stub_48,isr_stub_49,isr_stub_50,isr_stub_51,isr_stub_52,isr_stub_53,isr_stub_54,isr_stub_55,
    isr_stub_56,isr_stub_57,isr_stub_58,isr_stub_59,isr_stub_60,isr_stub_61,isr_stub_62,isr_stub_63,
    isr_stub_64,isr_stub_65,isr_stub_66,isr_stub_67,isr_stub_68,isr_stub_69,isr_stub_70,isr_stub_71,
    isr_stub_72,isr_stub_73,isr_stub_74,isr_stub_75,isr_stub_76,isr_stub_77,isr_stub_78,isr_stub_79,
    isr_stub_80,isr_stub_81,isr_stub_82,isr_stub_83,isr_stub_84,isr_stub_85,isr_stub_86,isr_stub_87,
    isr_stub_88,isr_stub_89,isr_stub_90,isr_stub_91,isr_stub_92,isr_stub_93,isr_stub_94,isr_stub_95,
    isr_stub_96,isr_stub_97,isr_stub_98,isr_stub_99,isr_stub_100,isr_stub_101,isr_stub_102,isr_stub_103,
    isr_stub_104,isr_stub_105,isr_stub_106,isr_stub_107,isr_stub_108,isr_stub_109,isr_stub_110,isr_stub_111,
    isr_stub_112,isr_stub_113,isr_stub_114,isr_stub_115,isr_stub_116,isr_stub_117,isr_stub_118,isr_stub_119,
    isr_stub_120,isr_stub_121,isr_stub_122,isr_stub_123,isr_stub_124,isr_stub_125,isr_stub_126,isr_stub_127,
    isr_stub_128,isr_stub_129,isr_stub_130,isr_stub_131,isr_stub_132,isr_stub_133,isr_stub_134,isr_stub_135,
    isr_stub_136,isr_stub_137,isr_stub_138,isr_stub_139,isr_stub_140,isr_stub_141,isr_stub_142,isr_stub_143,
    isr_stub_144,isr_stub_145,isr_stub_146,isr_stub_147,isr_stub_148,isr_stub_149,isr_stub_150,isr_stub_151,
    isr_stub_152,isr_stub_153,isr_stub_154,isr_stub_155,isr_stub_156,isr_stub_157,isr_stub_158,isr_stub_159,
    isr_stub_160,isr_stub_161,isr_stub_162,isr_stub_163,isr_stub_164,isr_stub_165,isr_stub_166,isr_stub_167,
    isr_stub_168,isr_stub_169,isr_stub_170,isr_stub_171,isr_stub_172,isr_stub_173,isr_stub_174,isr_stub_175,
    isr_stub_176,isr_stub_177,isr_stub_178,isr_stub_179,isr_stub_180,isr_stub_181,isr_stub_182,isr_stub_183,
    isr_stub_184,isr_stub_185,isr_stub_186,isr_stub_187,isr_stub_188,isr_stub_189,isr_stub_190,isr_stub_191,
    isr_stub_192,isr_stub_193,isr_stub_194,isr_stub_195,isr_stub_196,isr_stub_197,isr_stub_198,isr_stub_199,
    isr_stub_200,isr_stub_201,isr_stub_202,isr_stub_203,isr_stub_204,isr_stub_205,isr_stub_206,isr_stub_207,
    isr_stub_208,isr_stub_209,isr_stub_210,isr_stub_211,isr_stub_212,isr_stub_213,isr_stub_214,isr_stub_215,
    isr_stub_216,isr_stub_217,isr_stub_218,isr_stub_219,isr_stub_220,isr_stub_221,isr_stub_222,isr_stub_223,
    isr_stub_224,isr_stub_225,isr_stub_226,isr_stub_227,isr_stub_228,isr_stub_229,isr_stub_230,isr_stub_231,
    isr_stub_232,isr_stub_233,isr_stub_234,isr_stub_235,isr_stub_236,isr_stub_237,isr_stub_238,isr_stub_239,
    isr_stub_240,isr_stub_241,isr_stub_242,isr_stub_243,isr_stub_244,isr_stub_245,isr_stub_246,isr_stub_247,
    isr_stub_248,isr_stub_249,isr_stub_250,isr_stub_251,isr_stub_252,isr_stub_253,isr_stub_254,isr_stub_255,
};

void isr_install_all(void) {
    pic_remap();
    pic_disable();  // APIC handles IRQs; mask the 8259 completely
    for (int i = 0; i < 256; i++) {
        uint8_t dpl = (i == 3) ? IDT_DPL_USER : IDT_DPL_KERNEL;
        idt_set_gate((uint8_t)i, stub_table[i], IDT_GATE_INT, dpl, IDT_IST_NONE);
    }
}
