#ifndef __COMPAT_SOC_GPIO_H__
#define __COMPAT_SOC_GPIO_H__
/* Ingenic vendor GPIO enums (from 3.10 arch/mips/xburst/soc-t31), needed by the
   open-isp driver's private_jzgpio_* MCLK-mux helpers. Mainline 6.12 has no
   soc/gpio.h, so we provide just the enums; the actual register pokes in
   private_jzgpio_set_func() use ioremap'd GPIO regs directly. */
enum gpio_function {
	GPIO_FUNC_0 = 0x00, GPIO_FUNC_1 = 0x01, GPIO_FUNC_2 = 0x02, GPIO_FUNC_3 = 0x03,
	GPIO_OUTPUT0 = 0x04, GPIO_OUTPUT1 = 0x05, GPIO_INPUT = 0x06,
	GPIO_INT_LO = 0x08, GPIO_INT_HI = 0x09, GPIO_INT_FE = 0x0a, GPIO_INT_RE = 0x0b,
	GPIO_PULL_HIZ = 0x80, GPIO_PULL_UP = 0x90, GPIO_PULL_DOWN = 0xa0,
	GPIO_PULL_BUSHOLD = 0xb0, GPIO_PULL_DOWN_DIS = 0xc0, GPIO_INPUT_PULL_HI = 0x96
};
enum gpio_port { GPIO_PORT_A = 0, GPIO_PORT_B, GPIO_PORT_C, GPIO_NR_PORTS };


/* Ingenic GPIO pin-number macros (32 pins/port) — needed by sensor drivers'
   reset/pwdn gpio defaults (GPIO_PA(n) etc.). */
#ifndef GPIO_PA
#define GPIO_PA(n) (0*32 + (n))
#define GPIO_PB(n) (1*32 + (n))
#define GPIO_PC(n) (2*32 + (n))
#define GPIO_PD(n) (3*32 + (n))
#endif

#endif
