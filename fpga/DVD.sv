//============================================================================
//
//  This program is free software; you can redistribute it and/or modify it
//  under the terms of the GNU General Public License as published by the Free
//  Software Foundation; either version 2 of the License, or (at your option)
//  any later version.
//
//  This program is distributed in the hope that it will be useful, but WITHOUT
//  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
//  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
//  more details.
//
//  You should have received a copy of the GNU General Public License along
//  with this program; if not, write to the Free Software Foundation, Inc.,
//  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
//
//============================================================================

module emu
(
	`include "sys/emu_ports.vh"
);

///////// Default values for ports not used in this core /////////

assign ADC_BUS  = 'Z;
assign USER_OUT = '1;
assign {UART_RTS, UART_TXD, UART_DTR} = 0;
assign {SD_SCK, SD_MOSI, SD_CS} = 'Z;
assign {SDRAM_DQ, SDRAM_A, SDRAM_BA, SDRAM_CLK, SDRAM_CKE, SDRAM_DQML, SDRAM_DQMH, SDRAM_nWE, SDRAM_nCAS, SDRAM_nRAS, SDRAM_nCS} = 'Z;  

assign VGA_SL = 0;
assign VGA_SCALER  = 0;
assign VGA_DISABLE = 0;
assign HDMI_FREEZE = 0;
assign HDMI_BLACKOUT = 0;
// HDMI/ascal only. Analog VGA/CRT still uses the native 15 kHz interlaced
// raster (VGA_HS/VS/DE + VGA_F1). ascal bob-deinterlaces captured fields
// into a progressive feed for the HDMI scaler. Ignored when ascal has not
// detected interlaced input (i_inter=0).
assign HDMI_BOB_DEINT = 1;

assign AUDIO_S = 0;
assign AUDIO_L = 0;
assign AUDIO_R = 0;
assign AUDIO_MIX = 0;

assign LED_DISK = 0;
assign LED_POWER = 0;
assign BUTTONS = 0;

/////////////////  Native scanout (one VGA_* stream for CRT and HDMI)  /////
//
// CRT:  native 480i/576i from mycore + fb_line_reader. Timing unchanged.
// HDMI: same VGA_* capture into ascal, with HDMI_BOB_DEINT so the scaler
//       presents bob-deinterlaced progressive frames (not ARM 720p/1080p).
// VGA_F1 must remain the raster field so ascal detects interlaced input.
//
// Two native 720x576 BGR0/XRGB8888 or planar YUV420 buffers in reserved DDR:
//   A = 0x30000000   B = 0x30200000
// ARM requests a flip by writing the mailbox word at 0x30400000:
//   bit0 = A/B request
//   bit1 = pixel format (0=legacy BGR0, 1=planar YUV420)
//   bit2 = frame interlaced_frame (chroma row map; latched with A/B)
//   bit3 = top_field_first RESERVED (latched, unused for field order)
// The core polls that 64-bit word every 16384 cycles of 27 MHz clk_sys
// (~0.61 ms). mb_* always hold the latest observed request.
// display_buf/yuv/intl/tff latch that word together at the native
// complete-frame wrap, before field-0 line-0 prefetch. Both analogue
// fields and ASCAL capture of VGA_* use that one buffer for the frame.
// Logical controller state is published at 0x30400008
// (never written to 0x30400000): {JOY_MAGIC, display_buf, joystick_0[30:0]}.
// joystick bits 0-9 are unchanged. Bits 10-11 are Subtitle / Audio Next.
// Bit 31 is display_buf (A=0, B=1).
//
// Additional v1 registers (do not alias the mailbox/joystick words):
//   0x30400010 FPGA→ARM settings  {DVD2, ver, seq, tv_osd, crt, av, src}
//   0x30400018 ARM→FPGA control   {DVD3, source_std}
// Old player ignores these addresses. New player probes DVD2 liveness.
//
//////////////////////////////////////////////////////////////////

wire [1:0] ar = status[122:121];

assign VIDEO_ARX = (!ar) ? 12'd4 : (ar - 1'd1);
assign VIDEO_ARY = (!ar) ? 12'd3 : 12'd0;

`include "build_id.v"
// Status Bit Map:
//             Upper                             Lower
// 0         1         2         3          4         5         6
// 01234567890123456789012345678901 23456789012345678901234567890123
// 0123456789ABCDEFGHIJKLMNOPQRSTUV 0123456789ABCDEFGHIJKLMNOPQRSTUV
// X XX XX XXX      XXXXX                                  XX
// 0=reset  [2:1]=TV Mode  [4:3]=Noise  5/6-7/10=template
// 9=CRT Stabilizer  [16:12]=A/V Sync  [122:121]=AR
// status[8] is unused (legacy OSD Buffer A/B override removed).
//
// TV Mode: 0=Auto 1=NTSC 2=PAL. Fresh default Auto (O, first item).
// CRT Stabilizer: listed On,Off so status[9]=0 is On (MiSTer default-0).
//   dup_even = ~status[9]. Duplicate Even RTL is unchanged.
// A/V Sync 5-bit circular (Main_MiSTer LEFT/RIGHT wrap, mask=31):
//   raw 0 = 0 ms, 1..10 = +10..+100, 11..20 = -100..-10
//   RIGHT from 0 → 1 (+10). LEFT from 0 → 20 (-10).
localparam CONF_STR = {
	"DVD;;",
	"-;",
	"O[122:121],Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
	"O[2:1],TV Mode,Auto,NTSC,PAL;",
	"O[9],CRT Stabilizer,On,Off;",
	"O[16:12],A/V Sync,0 ms,+10 ms,+20 ms,+30 ms,+40 ms,+50 ms,+60 ms,+70 ms,+80 ms,+90 ms,+100 ms,-100 ms,-90 ms,-80 ms,-70 ms,-60 ms,-50 ms,-40 ms,-30 ms,-20 ms,-10 ms;",
	"O[4:3],Noise,White,Red,Green,Blue;",
	"-;",
	"P1,Test Page 1;",
	"P1-;",
	"P1-, -= Options in page 1 =-;",
	"P1-;",
	"P1O[5],Option 1-1,Off,On;",
	"d0P1F1,BIN;",
	"H0P1O[10],Option 1-2,Off,On;",
	"-;",
	"P2,Test Page 2;",
	"P2-;",
	"P2-, -= Options in page 2 =-;",
	"P2-;",
	"P2S0,DSK;",
	"P2O[7:6],Option 2,1,2,3,4;",
	"-;",
	"-;",
	"T[0],Reset;",
	"R[0],Reset and close OSD;",
	// D-pad is implicit: joystick_0[0]=Right [1]=Left [2]=Down [3]=Up.
	// Named buttons occupy bits 4-11. Do not map OSD/Home (buttons[0]).
	// J1[0] is Confirm (jn/jp A). It is NOT named Select, so SYS_BTN_SELECT
	// (Minus) cannot bind here. Audio Next's jn/jp default is Select.
	"J1,Confirm,Back,Play/Pause,DVD Menu,Previous Chapter,Next Chapter,Subtitle,Audio Next;",
	"jn,A,B,Start,X,L,R,Y,Select;",
	"jp,A,B,Start,X,L,R,Y,Select;",
	"v,2;", // bumped: drop OSD Buffer, Confirm rename, Subtitle/Audio Next
	"V,v",`BUILD_DATE 
};

wire forced_scandoubler;
wire   [1:0] buttons;
wire [127:0] status;
wire  [10:0] ps2_key;
wire [31:0] joystick_0;

hps_io #(.CONF_STR(CONF_STR)) hps_io
(
	.clk_sys(clk_sys),
	.HPS_BUS(HPS_BUS),
	.EXT_BUS(),
	.gamma_bus(),

	.forced_scandoubler(forced_scandoubler),

	.buttons(buttons),
	.status(status),
	.status_menumask({status[5]}),
	.joystick_0(joystick_0),

	.ps2_key(ps2_key)
);

///////////////////////   CLOCKS   ///////////////////////////////

wire clk_sys;
pll pll
(
	.refclk(CLK_50M),
	.rst(0),
	.outclk_0(clk_sys)
);

// Mailbox at physical 0x30400000. DDRAM_ADDR is byte_addr[31:3].
// Poll 16384 cycles of 27 MHz (~0.61 ms).
// 0x30400000 bit0 = ARM→FPGA FB A/B request (never RMW / never FPGA-write).
// 0x30400000 bit1 = ARM→FPGA pixel format (0=legacy BGR0, 1=planar YUV420).
// 0x30400000 bit2 = ARM→FPGA frame interlaced_frame (latched with bit0).
// 0x30400000 bit3 = ARM→FPGA top_field_first RESERVED (latched, unused).
// 0x30400008       = FPGA→ARM {JOY_MAGIC, display_buf, joystick_0[30:0]}.
// 0x30400010       = FPGA→ARM settings/capability (DVD2). Never written by ARM.
// 0x30400018       = ARM→FPGA source-standard control (DVD3). FPGA reads only.
// One mailbox-read opportunity is latched every poll tick (poll_due).
// Video has exclusive use of DDRAM only while a burst is in flight
// (vid_active). fill_need must not starve mailbox/status: otherwise
// back-to-back line fills (or a stuck ST_WAIT) freeze DVD2 set_seq and
// never resample ARM's A/B request. Follow-up ctl/set/joy beats also
// block new video starts so a poll can finish publishing DVD1/DVD2.
// Extra settings/control beats return to idle between ops so video can
// preempt after the poll completes.
localparam [28:0] MB_ADDR    = 29'h0608_0000; // 0x30400000
localparam [28:0] JOY_ADDR   = 29'h0608_0001; // 0x30400008
localparam [28:0] SET_ADDR   = 29'h0608_0002; // 0x30400010
localparam [28:0] CTL_ADDR   = 29'h0608_0003; // 0x30400018
localparam [13:0] POLL_MAX   = 14'd16383;
localparam [31:0] JOY_MAGIC  = 32'h44564431;  // "DVD1"
localparam [31:0] SET_MAGIC  = 32'h44564432;  // "DVD2"
localparam [31:0] CTL_MAGIC  = 32'h44564433;  // "DVD3"
localparam [7:0]  SET_VER    = 8'd1;
localparam [9:0]  JOY_HB_MAX = 10'd1023;      // heartbeat ~0.62 s at 27 MHz

localparam ST_IDLE     = 4'd0;
localparam ST_RD_MB    = 4'd1;
localparam ST_RD_MB_W  = 4'd2;
localparam ST_WR_JOY   = 4'd3;
localparam ST_WR_JOY_H = 4'd4;
localparam ST_WR_SET   = 4'd5;
localparam ST_WR_SET_H = 4'd6;
localparam ST_RD_CTL   = 4'd7;
localparam ST_RD_CTL_W = 4'd8;

reg        mb_rd  = 0;
reg        mb_we  = 0;
reg        mb_bit = 0;
reg        mb_yuv = 0;
reg        mb_intl = 0;
reg        mb_tff  = 0;
reg  [3:0] mb_st  = 0;
reg [13:0] poll_cnt = 0;
reg        poll_due = 0;
reg        ctl_due  = 0;
reg        set_due  = 0;
reg        joy_pending = 1'b1;
reg [31:0] joy_sent = 32'hffff_ffff;
reg        disp_sent = 1'b0;
reg  [9:0] joy_hb = 0;
reg  [7:0] set_seq = 0;
reg  [1:0] src_std = 2'd0;

wire        vid_req;
wire        vid_active;
wire        vid_rd;
wire [28:0] vid_addr;
wire  [7:0] vid_burstcnt;
wire  [7:0] pix_r, pix_g, pix_b;
wire        display_buf;

// New video bursts wait while mailbox is busy or a poll/follow-up is due.
// In-flight video (vid_active) still owns DDRAM until the burst completes.
wire        mb_allow_vid = (mb_st == ST_IDLE) && !poll_due && !ctl_due &&
                           !set_due && !joy_pending;

// OSD TV Mode status[2:1]: 0 Auto, 1 NTSC, 2 PAL.
// CRT: CONF_STR On,Off so status[9]==0 is On; invert for existing dup_even.
// Auto + UNKNOWN/NTSC → NTSC (launcher-safe). Auto + PAL → PAL.
wire [1:0] tv_osd = status[2:1];
wire       crt_on = ~status[9];
wire [4:0] av_raw = status[16:12];
wire       pal_eff = (tv_osd == 2'd2) ? 1'b1 :
                     (tv_osd == 2'd1) ? 1'b0 :
                     (src_std == 2'd2);

wire [63:0] joy_word = {JOY_MAGIC, display_buf, joystick_0[30:0]};
// DVD2 pad bit2 = YUV420 reader capability (this SET word, not the mailbox).
// Mailbox 0x30400000 bit2 is frame interlaced_frame — different address.
// SET_VER stays 1 so ARM v1 probe still matches. Bits [7:3] remain 0.
wire [63:0] set_word = {SET_MAGIC, SET_VER, set_seq, tv_osd, crt_on, av_raw, 5'd0, 1'b1, src_std};

wire [28:0] mb_addr =
	(mb_st == ST_WR_JOY || mb_st == ST_WR_JOY_H) ? JOY_ADDR :
	(mb_st == ST_WR_SET || mb_st == ST_WR_SET_H) ? SET_ADDR :
	(mb_st == ST_RD_CTL || mb_st == ST_RD_CTL_W) ? CTL_ADDR :
	MB_ADDR;

assign DDRAM_CLK      = clk_sys;
assign DDRAM_BURSTCNT = vid_active ? vid_burstcnt : 8'd1;
assign DDRAM_ADDR     = vid_active ? vid_addr : mb_addr;
assign DDRAM_DIN      = (mb_st == ST_WR_SET || mb_st == ST_WR_SET_H) ? set_word : joy_word;
assign DDRAM_BE       = 8'hFF;
assign DDRAM_WE       = vid_active ? 1'b0 : mb_we;
assign DDRAM_RD       = vid_active ? vid_rd : mb_rd;

always @(posedge clk_sys) begin
	mb_rd <= 0;
	mb_we <= 0;
	poll_cnt <= poll_cnt + 1'd1;

	if (poll_cnt == POLL_MAX)
		poll_due <= 1'b1;

	case (mb_st)
		ST_IDLE: if (vid_active) begin
				// In-flight video burst owns DDRAM until it completes.
			end else if (poll_due) begin
				poll_due <= 1'b0;
				joy_hb <= joy_hb + 1'd1;
				if (joy_hb == JOY_HB_MAX) begin
					joy_hb <= 10'd0;
					joy_pending <= 1'b1;
				end
				mb_st <= ST_RD_MB;
			end else if (ctl_due) begin
				mb_st <= ST_RD_CTL;
			end else if (set_due) begin
				mb_st <= ST_WR_SET;
			end else if (joy_pending) begin
				mb_st <= ST_WR_JOY;
			end
		ST_RD_MB: if (!DDRAM_BUSY) begin
				mb_rd <= 1;
				mb_st <= ST_RD_MB_W;
			end
		ST_RD_MB_W: if (DDRAM_DOUT_READY) begin
				mb_bit  <= DDRAM_DOUT[0];
				mb_yuv  <= DDRAM_DOUT[1];
				mb_intl <= DDRAM_DOUT[2];
				mb_tff  <= DDRAM_DOUT[3];
				ctl_due <= 1'b1;
				set_due <= 1'b1;
				mb_st  <= ST_IDLE;
			end
		ST_WR_JOY: if (!DDRAM_BUSY) begin
				mb_we <= 1;
				mb_st <= ST_WR_JOY_H;
			end
		ST_WR_JOY_H: begin
				joy_sent  <= joystick_0;
				disp_sent <= display_buf;
				joy_pending <= 1'b0;
				mb_st <= ST_IDLE;
			end
		ST_WR_SET: if (!DDRAM_BUSY) begin
				mb_we <= 1;
				mb_st <= ST_WR_SET_H;
			end
		ST_WR_SET_H: begin
				set_seq <= set_seq + 8'd1;
				set_due <= 1'b0;
				mb_st <= ST_IDLE;
			end
		ST_RD_CTL: if (!DDRAM_BUSY) begin
				mb_rd <= 1;
				mb_st <= ST_RD_CTL_W;
			end
		ST_RD_CTL_W: if (DDRAM_DOUT_READY) begin
				if (DDRAM_DOUT[63:32] == CTL_MAGIC && DDRAM_DOUT[1:0] <= 2'd2)
					src_std <= DDRAM_DOUT[1:0];
				ctl_due <= 1'b0;
				mb_st <= ST_IDLE;
			end
		default: mb_st <= ST_IDLE;
	endcase

	if (joystick_0 != joy_sent)
		joy_pending <= 1'b1;
	if (display_buf != disp_sent)
		joy_pending <= 1'b1;
end

wire reset = RESET | status[0] | buttons[1];

wire [1:0] col = status[4:3];

wire HBlank;
wire HSync;
wire VBlank;
wire VSync;
wire ce_pix;
wire hvcnt_atzero;
wire field;
wire [9:0] hc;
wire [9:0] vc;

// Leave H/V sync always on. This stabilizes the video output while the core
// is in reset. This example releases the reset when H/V counters are at zero.
reg reset_core = 1;
always @(posedge clk_sys) begin
	if(reset) reset_core <= 1;
	else if(hvcnt_atzero) reset_core <= 0;
end

mycore mycore
(
	.clk(clk_sys),
	.reset(reset_core),

	.pal(pal_eff),
	.scandouble(forced_scandoubler),

	.ce_pix(ce_pix),
	.hvcnt_atzero(hvcnt_atzero),

	.HBlank(HBlank),
	.HSync(HSync),
	.VBlank(VBlank),
	.VSync(VSync),
	.field(field),
	.hc(hc),
	.vc(vc)
);

fb_line_reader fb_line_reader
(
	.clk(clk_sys),
	.reset(reset),

	.hc(hc),
	.vc(vc),
	.field(field),
	.ce_pix(ce_pix),
	.req_buf(mb_bit),
	.req_yuv(mb_yuv),
	.req_intl(mb_intl),
	.req_tff(mb_tff),
	.display_buf(display_buf),
	.display_yuv(),
	.display_intl(),
	.display_tff(),
	.pal(pal_eff),
	.dup_even(crt_on),

	.mb_idle(mb_allow_vid),
	.vid_req(vid_req),
	.vid_active(vid_active),
	.ddr_rd(vid_rd),
	.ddr_addr(vid_addr),
	.ddr_burstcnt(vid_burstcnt),
	.DDRAM_BUSY(DDRAM_BUSY),
	.DDRAM_DOUT(DDRAM_DOUT),
	.DDRAM_DOUT_READY(DDRAM_DOUT_READY),

	.pix_r(pix_r),
	.pix_g(pix_g),
	.pix_b(pix_b)
);

assign CLK_VIDEO = clk_sys;
assign CE_PIXEL = ce_pix;
assign VGA_F1 = field;

assign VGA_DE = ~(HBlank | VBlank);
assign VGA_HS = HSync;
assign VGA_VS = VSync;
assign VGA_G  = (!col || col == 2) ? pix_g : 8'd0;
assign VGA_R  = (!col || col == 1) ? pix_r : 8'd0;
assign VGA_B  = (!col || col == 3) ? pix_b : 8'd0;

reg  [26:0] act_cnt;
always @(posedge clk_sys) act_cnt <= act_cnt + 1'd1; 
assign LED_USER    = act_cnt[26]  ? act_cnt[25:18]  > act_cnt[7:0]  : act_cnt[25:18]  <= act_cnt[7:0];

endmodule
