#!/usr/bin/env python3
"""hakios.mic.c (POCO X5 Pro, redwood) into the Redmi K40 overlay, made the K40's (kona, SM8250).

What differs, from the K40's live device tree and Xiaomi's mixer_paths.xml (work/fw/audiopack), with
mainline Linux as the reference for the kona parts (sm8250.dtsi swr2, lpass-tx-macro.c, wcd938x.c,
drivers/soundwire/qcom.c):
  - the codec's reset is GPIO32 (msm_cdc_pinctrl@32), in the TLMM's south tile at 0x0f500000 (mainline
    pinctrl-sm8250.c: PINGROUP(32, SOUTH, ...)), not redwood's GPIO83 at 0x0f100000;
  - the TX SoundWire bus has three pins, LPASS gpio0 (clock, no pull) and gpio1/gpio2 (data, keeper);
    the LPASS pin controller has gpio0-13 only (redwood's fourth pin, gpio14, does not exist);
  - the TX SoundWire master is v1.5.1 (no HCTL block at 0x032a9000: never touched);
  - the digital codec is a bolero 1.x (redwood's 2.0/2.1 check refused it): accepted, and the SoundWire
    mics' clock dividers left as reset has them, as mainline leaves them on sm8250;
  - the main microphone (Xiaomi's "main-mic"): ADC1 -> "TX SMIC MUX0" ADC0 -> decimator 0 ("TX DEC0 MUX"
    SWR_MIC) -> TX_CDC_DMA_TX_3, "ADC1 Volume" 20; redwood's decimator 1 and MUX1 become 0;
  - ADC1 in NORMAL mode (0x03) with the codec's TX clock rate 9.6 MHz (0), mainline's default;
  - its port: the codec's DP1 to the master's port 2 (sm8250 boards: qcom,tx-port-mapping <2 3 4 5>;
    the K40 tree: ADC1 on port 2), sample interval field 1, offset1 1, offset2 0, lane 0 on both sides
    (sm8250.dtsi swr2 qcom,ports-*), the frame 50 x 16 at MCLK / 1.
Nothing runs on any board but alioth."""
import pathlib
import sys

hk = pathlib.Path.home() / 'hk'
src = hk / 'xnu/files/bsd/dev/hakios.mic.c'      # (the upstream tree's, as git has it; out/ has the overlay's copy after a build)
dst = hk / 'devices/redmi-k40/overlays/xnu/files/bsd/dev/hakios.mic.c'
s = src.read_text()
if 'alioth: the K40' in s:
    sys.exit('already patched source?')


def rep(old, new, count=1):
    global s
    n = s.count(old)
    if n != count:
        sys.exit('expected %d of %r, found %d' % (count, old[:80], n))
    s = s.replace(old, new)


# the blocks
rep('static struct block tlmm = { "the pin controller", 0x0f100000, 0x100000 };',
    'static struct block tlmm = { "the pin controller", 0x0f500000, 0x100000 };     /* (alioth: the TLMM\'s south tile, GPIO32\'s) */')
rep('''	(void)get(&lpi_high, "gpio14 config (SoundWire TX data 2)", 0);
	(void)get(&lpi_high, "gpio14 in/out", 4);
	(void)get(&tlmm, "gpio83 config (the codec's reset)", 0x1000 * 83);
	(void)get(&tlmm, "gpio83 in/out", 0x1000 * 83 + 4);''',
    '''	(void)get(&tlmm, "gpio32 config (the codec's reset)", 0x1000 * 32);
	(void)get(&tlmm, "gpio32 in/out", 0x1000 * 32 + 4);''')
rep('''	(void)get(&tx_hctl, "HCTL (0x32a90a8)", 0x0a8);
}''', '''}''')

# the codec's reset: GPIO32
rep('#define TLMM_CFG_83             (0x1000 * 83)\n#define TLMM_IO_83              (0x1000 * 83 + 4)',
    '#define TLMM_CFG_83             (0x1000 * 32)   /* (alioth: GPIO32, the name kept) */\n'
    '#define TLMM_IO_83              (0x1000 * 32 + 4)')
s = s.replace('"gpio83 ', '"gpio32 ')

# three SoundWire pins
rep('''	{ 0, 0, 0, "gpio0 (SoundWire TX clock)" }, { 1, 2, 2, "gpio1 (data 0)" }, { 2, 4, 2, "gpio2 (data 1)" },
	{ 14, 6, 2, "gpio14 (data 2)" },''',
    '''	{ 0, 0, 0, "gpio0 (SoundWire TX clock)" }, { 1, 2, 2, "gpio1 (data 0)" }, { 2, 4, 2, "gpio2 (data 1)" },''')
s = s.replace('"slew rates: 1 for the four"', '"slew rates: 1 for the three"')

# no HCTL on a v1.5.1 master
rep('''	if (tx_hctl.base == 0) {
		tx_hctl.base = ml_io_map(tx_hctl.phys, tx_hctl.size);
	}
	found_hctl = rd(tx_hctl.base + 0x0a8);
	master_taken = true;
	/* "SW workaround to gate hw_ctl for SWR version >= 1.6" */
	if (!put(&tx_hctl, "HCTL: bit 1 cleared", 0x0a8, found_hctl & ~2u, 0x3)) {
		return false;
	}
	spin_us(505);''',
    '''	master_taken = true;            /* (alioth: a v1.5.1 master, without the HCTL of 1.6 and later) */''')
rep('''	(void)put(&tx_hctl, "HCTL, as it was found", 0x0a8, found_hctl, 0x3);
	master_taken = false;''', '''	master_taken = false;''')

# bolero 1.x accepted
rep('''	bolero_version = id0 == 0x01 && id1 == 0x0f ? 20 : id0 == 0x02 && id1 == 0x0e ? 21 : 0;
	say("mic: the digital codec is bolero %s\\n", bolero_version == 20 ? "2.0" : bolero_version == 21 ? "2.1" : "OF AN UNKNOWN VERSION");
	if (bolero_version != 21) {
		return bolero_version == 20;
	}''',
    '''	bolero_version = id0 == 0x01 && id1 == 0x0f ? 20 : id0 == 0x02 && id1 == 0x0e ? 21 : 10;
	/* (alioth: kona's bolero is a 1.x, core ID other than 2.0's and 2.1's; its SoundWire mics' clock dividers are left as reset
	 * has them, as mainline Linux leaves them on sm8250) */
	say("mic: the digital codec is bolero %s (core ID %#x %#x)\\n", bolero_version == 20 ? "2.0" : bolero_version == 21 ? "2.1" : "1.x",
	    id0, id1);
	if (bolero_version != 21) {
		return true;
	}''')

# decimator 0 from ADC mux 0
rep('''#define TX_ADC_MUX1_CFG0                0x108
#define TX_ADC_MUX1_CFG1                0x10c
#define TX1_PATH_CTL                    0x480
#define TX1_PATH_CFG0                   0x484
#define TX1_VOL_CTL                     0x48c
#define TX1_PATH_SEC2                   0x498''',
    '''/* (alioth: Xiaomi's main-mic path is decimator 0 from ADC mux 0; the names of redwood's decimator 1 kept) */
#define TX_ADC_MUX1_CFG0                0x100
#define TX_ADC_MUX1_CFG1                0x104
#define TX1_PATH_CTL                    0x400
#define TX1_PATH_CFG0                   0x404
#define TX1_VOL_CTL                     0x40c
#define TX1_PATH_SEC2                   0x418''')

# decimator 0's own init (tx_macro_component_probe(): TX0_TX_PATH_SEC7 [5:0] 0x0A), kept with the others and put back
rep('static uint32_t found_dec[10], found_va_mic[3];', 'static uint32_t found_dec[11], found_va_mic[3];')
rep('''	TX_SWR_MIC2_CTL + 12, TX_ADC_MUX1_CFG0, TX_ADC_MUX1_CFG1, TX1_PATH_CTL,
};''', '''	TX_SWR_MIC2_CTL + 12, TX_ADC_MUX1_CFG0, TX_ADC_MUX1_CFG1, TX1_PATH_CTL, TX0_PATH_SEC7,
};''')
rep('#define TX1_PATH_SEC2                   0x418\n',
    '#define TX1_PATH_SEC2                   0x418\n#define TX0_PATH_SEC7                   0x42c   /* (alioth) */\n')
rep('''	if (!put_bits(&tx_macro, "TX1_TX_PATH_CFG0: an analog mic", TX1_PATH_CFG0, 0x80, 0x00) ||''',
    '''	if (!put_bits(&tx_macro, "TX0_TX_PATH_SEC7: 0xa (tx_macro_component_probe())", TX0_PATH_SEC7, 0x3f, 0x0a) ||
	    !put_bits(&tx_macro, "TX1_TX_PATH_CFG0: an analog mic", TX1_PATH_CFG0, 0x80, 0x00) ||''')
# the codec DMA's channel: decimator 0's bit (tx_macro_get_channel_map(): active_ch_mask has the decimators' bits)
rep('config = { 1, MIC_RATE, 16, 0, 1, 1u << 1 };', 'config = { 1, MIC_RATE, 16, 0, 1, 1u << 0 };    /* (alioth: decimator 0) */')
rep('"the port TX_CDC_DMA_TX_3 (0xb037): 1 channel of 16 bits at 48 kHz, decimator 1"',
    '"the port TX_CDC_DMA_TX_3 (0xb037): 1 channel of 16 bits at 48 kHz, decimator 0"')

# ADC1: gain 20, NORMAL mode
rep('''	       /* "ADC1 Volume" 6 (Xiaomi's mixer paths) */
	       codec_bits(WCD_ANA_TX_CH1, 0x1f, 6) &&''',
    '''	       /* "ADC1 Volume" 20 (the K40's main-mic path) */
	       codec_bits(WCD_ANA_TX_CH1, 0x1f, 20) &&''')
rep('codec_bits(WCD_CDC_TX_ANA_MODE_0_1, 0x0f, 0x05)',
    'codec_bits(WCD_CDC_TX_ANA_MODE_0_1, 0x0f, 0x03) /* (alioth: NORMAL, mainline\'s default) */')

# the ports: codec DP1 interval field 1, offset1 1, lane 0; master port 2 the same
rep('''	uint8_t codec_interval = main_timing ? 7 : 3, master_interval = 7;''',
    '''	/* (alioth: sm8250.dtsi swr2's port 2 -- the codec's DP1, ADC1 -- sample interval field 1, offset1 1, lane 0 on both sides) */
	uint8_t codec_interval = 1, master_interval = 1, offset1 = 1, lane = 0;''')
rep('''	swr_write(codec_device, SWRS_DP_OFFSET_CONTROL_1_BANK(0, bank), 0, write_id);''',
    '''	swr_write(codec_device, SWRS_DP_OFFSET_CONTROL_1_BANK(0, bank), offset1, write_id);''')
rep('''	swr_write(codec_device, SWRS_DP_LANE_CONTROL_BANK(0, bank), 1, write_id);
	note("mic: the codec's data port 1, bank %u: channel 1 on, sample interval %u, offsets 0 0, lane 1\\n", bank, codec_interval);
	/* the master's port 1 */
	return put(&tx_swr, "DP_PORT_CTRL (port 1): channel 1, offsets 0 0, interval 7", SWRM_DP_PORT_CTRL_BANK(1, bank),
	           0x01u << 24 | 0u << 16 | 0u << 8 | master_interval, 0xffffffff) &&
	       put(&tx_swr, "DP_SAMPLECTRL2 (port 1)", SWRM_DP_SAMPLECTRL2_BANK(1, bank), 0, 0xff) &&
	       put(&tx_swr, "DP_PORT_CTRL_2 (port 1): lane 1", SWRM_DP_PORT_CTRL_2_BANK(1, bank), 1, 0xff) &&
	       /* (it reads 0 back on the phone whatever is written; Android writes it and never reads it) */
	       put(&tx_swr, "DP_PORT_HCTRL (port 1): hstop 15, hstart 0", SWRM_DP_PORT_HCTRL_BANK(1, bank), 0xf0, 0);''',
    '''	swr_write(codec_device, SWRS_DP_LANE_CONTROL_BANK(0, bank), lane, write_id);
	note("mic: the codec's data port 1, bank %u: channel 1 on, sample interval field %u, offsets %u 0, lane %u\\n", bank, codec_interval,
	    offset1, lane);
	/* the master's port 2 */
	return put(&tx_swr, "DP_PORT_CTRL (port 2): channel 1, offset1 1, interval field 1", SWRM_DP_PORT_CTRL_BANK(2, bank),
	           0x01u << 24 | 0u << 16 | (uint32_t)offset1 << 8 | master_interval, 0xffffffff) &&
	       put(&tx_swr, "DP_SAMPLECTRL2 (port 2)", SWRM_DP_SAMPLECTRL2_BANK(2, bank), 0, 0xff) &&
	       put(&tx_swr, "DP_PORT_CTRL_2 (port 2): lane 0", SWRM_DP_PORT_CTRL_2_BANK(2, bank), lane, 0xff);''')
# the K40's only
rep('#include <sys/hakios.audio.h>\n', '#include <sys/hakios.audio.h>\n#include <sys/hakios.board.h>\n')
rep('''	if (!PE_parse_boot_argn("mic", &level, sizeof(level)) || level == 0) {
		return;
	}''',
    '''	if (!PE_parse_boot_argn("mic", &level, sizeof(level)) || level == 0) {
		return;
	}
	if (!hakios_board_is_alioth()) {
		/* (alioth: every address here is the Redmi K40's; on another phone nothing is touched) */
		say("mic: this is the Redmi K40's microphone path, and this is not a Redmi K40: nothing done\\n");
		return;
	}''')
dst.write_text(s)
print('wrote', dst, len(s.splitlines()), 'lines')
