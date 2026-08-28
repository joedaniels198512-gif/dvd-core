// SPDX-License-Identifier: GPL-2.0-or-later
// BT.601 limited-range YCbCr → RGB, integer formula matching DVD swscale.
//
//   C = Y - 16
//   D = U - 128
//   E = V - 128
//   R = sat8((298*C + 409*E + 128) >> 8)
//   G = sat8((298*C - 100*D - 208*E + 128) >> 8)
//   B = sat8((298*C + 516*D + 128) >> 8)
//
// Combinational. Callers register Y/U/V so this sits in the existing
// 1-cycle pixel path (same as BGR0 pair_d). VGA_R/G/B order: R, G, B.

module yuv601_rgb
(
	input      [7:0] y,
	input      [7:0] u,
	input      [7:0] v,
	output     [7:0] r,
	output     [7:0] g,
	output     [7:0] b
);

reg signed [8:0]  C, D, E;
reg signed [19:0] r_acc, g_acc, b_acc;
reg signed [11:0] r_s, g_s, b_s;
reg        [7:0]  r_r, g_r, b_r;

always @* begin
	C = $signed({1'b0, y}) - 9'sd16;
	D = $signed({1'b0, u}) - 9'sd128;
	E = $signed({1'b0, v}) - 9'sd128;

	r_acc = 20'sd298 * C + 20'sd409 * E + 20'sd128;
	g_acc = 20'sd298 * C - 20'sd100 * D - 20'sd208 * E + 20'sd128;
	b_acc = 20'sd298 * C + 20'sd516 * D + 20'sd128;

	r_s = r_acc >>> 8;
	g_s = g_acc >>> 8;
	b_s = b_acc >>> 8;

	if (r_s < 12'sd0)
		r_r = 8'd0;
	else if (r_s > 12'sd255)
		r_r = 8'd255;
	else
		r_r = r_s[7:0];

	if (g_s < 12'sd0)
		g_r = 8'd0;
	else if (g_s > 12'sd255)
		g_r = 8'd255;
	else
		g_r = g_s[7:0];

	if (b_s < 12'sd0)
		b_r = 8'd0;
	else if (b_s > 12'sd255)
		b_r = 8'd255;
	else
		b_r = b_s[7:0];
end

assign r = r_r;
assign g = g_r;
assign b = b_r;

endmodule
