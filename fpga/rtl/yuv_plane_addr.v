// Planar YUV420P byte-address / 8 (DDRAM_ADDR) inside one 2 MiB A/B slot.
//
//   Y  +0x000000  stride 720   (90 beats / line)
//   U  +0x080000  stride 360   (45 beats / line)
//   V  +0x0A0000  stride 360   (45 beats / line)
//
// SIMPLE chroma siting: chroma_row = y >> 1.
// MPEG-2 interlaced 4:2:0 is field-aware; this first prototype is approximate.

module yuv_plane_addr
(
	input         display_buf,
	input  [9:0]  y,
	output [28:0] y_addr,
	output [28:0] u_addr,
	output [28:0] v_addr
);

localparam [28:0] FB_A_ADDR = 29'h0600_0000; // 0x30000000 >> 3
localparam [28:0] FB_B_ADDR = 29'h0604_0000; // 0x30200000 >> 3
localparam [28:0] U_OFF     = 29'h0001_0000; // 0x080000 >> 3
localparam [28:0] V_OFF     = 29'h0001_4000; // 0x0A0000 >> 3

wire [28:0] fb_base = display_buf ? FB_B_ADDR : FB_A_ADDR;
wire  [9:0] cy      = {1'b0, y[9:1]};

assign y_addr = fb_base + y  * 29'd90;
assign u_addr = fb_base + U_OFF + cy * 29'd45;
assign v_addr = fb_base + V_OFF + cy * 29'd45;

endmodule
