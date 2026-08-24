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
assign HDMI_BOB_DEINT = 0;

assign AUDIO_S = 0;
assign AUDIO_L = 0;
assign AUDIO_R = 0;
assign AUDIO_MIX = 0;

assign LED_DISK = 0;
assign LED_POWER = 0;
assign BUTTONS = 0;

/////////////////  Native scanout (one VGA_* stream for CRT and HDMI)  /////
//
// Two native 720x576 BGR0/XRGB8888 buffers in reserved DDR:
//   A = 0x30000000   B = 0x30200000
// ARM requests a flip by writing bit 0 of the mailbox word at 0x30400000.
// The core polls that 64-bit word every 16384 cycles of 27 MHz clk_sys
// (~0.61 ms). mb_bit always holds the latest observed request.
// display_buf latches mb_bit (OSD Buffer B still forces B) at the native
// complete-frame wrap, before field-0 line-0 prefetch. Both analogue
// fields and ASCAL capture of VGA_* use that one buffer for the frame.
// Logical controller state is published at 0x30400008
// (never written to 0x30400000): {JOY_MAGIC, display_buf, joystick_0[30:0]}.
// joystick bits 0-9 are unchanged. Bit 31 is display_buf (A=0, B=1).
//
//////////////////////////////////////////////////////////////////

wire [1:0] ar = status[122:121];

assign VIDEO_ARX = (!ar) ? 12'd4 : (ar - 1'd1);
assign VIDEO_ARY = (!ar) ? 12'd3 : 12'd0;

`include "build_id.v" 
localparam CONF_STR = {
	"DVD;;",
	"-;",
	"O[122:121],Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
	"O[8],Buffer,A,B;",
	"O[2],TV Mode,NTSC,PAL;",
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
	// Named buttons occupy bits 4-9. Do not map OSD/Home (buttons[0]).
	"J1,Select,Back,Play/Pause,DVD Menu,Previous Chapter,Next Chapter;",
	"jn,A,B,Start,X,L,R;",
	"jp,A,B,Start,X,L,R;",
	"v,0;", // [optional] config version 0-99.
	        // If CONF_STR options are changed in incompatible way, then change version number too,
			  // so all options will get default values on first start.
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
// 0x30400008       = FPGA→ARM {JOY_MAGIC, display_buf, joystick_0[30:0]}.
// One mailbox-read opportunity is latched every poll tick (poll_due) and
// always taken from idle before any controller write.
// Video line-fill has priority on DDRAM; mailbox runs when the reader is idle.
localparam [28:0] MB_ADDR    = 29'h0608_0000; // 0x30400000
localparam [28:0] JOY_ADDR   = 29'h0608_0001; // 0x30400008
localparam [13:0] POLL_MAX   = 14'd16383;
localparam [31:0] JOY_MAGIC  = 32'h44564431;  // "DVD1"
localparam [9:0]  JOY_HB_MAX = 10'd1023;      // heartbeat ~0.62 s at 27 MHz

reg        mb_rd  = 0;
reg        mb_we  = 0;
reg        mb_bit = 0;
reg  [2:0] mb_st  = 0; // 0 idle, 1 issue rd, 2 wait rd, 3 issue wr, 4 wr hold
reg [13:0] poll_cnt = 0;
reg        poll_due = 0;
reg        joy_pending = 1'b1;
reg [31:0] joy_sent = 32'hffff_ffff;
reg        disp_sent = 1'b0;
reg  [9:0] joy_hb = 0;

wire        vid_req;
wire        vid_active;
wire        vid_rd;
wire [28:0] vid_addr;
wire  [7:0] vid_burstcnt;
wire  [7:0] pix_r, pix_g, pix_b;
wire        display_buf;

assign DDRAM_CLK      = clk_sys;
assign DDRAM_BURSTCNT = vid_active ? vid_burstcnt : 8'd1;
assign DDRAM_ADDR     = vid_active ? vid_addr :
                        ((mb_st == 3'd3 || mb_st == 3'd4) ? JOY_ADDR : MB_ADDR);
assign DDRAM_DIN      = {JOY_MAGIC, display_buf, joystick_0[30:0]};
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
		0: if (vid_req) begin
				// Video owns or wants DDRAM; wait.
			end else if (poll_due) begin
				poll_due <= 1'b0;
				joy_hb <= joy_hb + 1'd1;
				if (joy_hb == JOY_HB_MAX) begin
					joy_hb <= 10'd0;
					joy_pending <= 1'b1;
				end
				mb_st <= 3'd1;
			end else if (joy_pending) begin
				mb_st <= 3'd3;
			end
		1: if (!DDRAM_BUSY) begin
				mb_rd <= 1;
				mb_st <= 3'd2;
			end
		2: if (DDRAM_DOUT_READY) begin
				mb_bit <= DDRAM_DOUT[0];
				mb_st  <= 3'd0;
			end
		3: if (!DDRAM_BUSY) begin
				mb_we <= 1;
				mb_st <= 3'd4;
			end
		4: begin
				joy_sent  <= joystick_0;
				disp_sent <= display_buf;
				joy_pending <= 1'b0;
				mb_st <= 3'd0;
			end
		default: mb_st <= 3'd0;
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

	.pal(status[2]),
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
	.req_buf(mb_bit | status[8]),
	.display_buf(display_buf),
	.pal(status[2]),

	.mb_idle(mb_st == 3'd0),
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
