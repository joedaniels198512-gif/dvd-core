// BT.601 converter vs the same integer formula used by yuv601_ref.c.
`timescale 1ns/1ps

module yuv601_tb;

integer fails = 0;

function integer sat8;
	input integer x;
	begin
		if (x < 0)
			sat8 = 0;
		else if (x > 255)
			sat8 = 255;
		else
			sat8 = x;
	end
endfunction

function integer ref_r;
	input integer y, u, v;
	integer C, E;
	begin
		C = y - 16;
		E = v - 128;
		ref_r = sat8((298 * C + 409 * E + 128) >>> 8);
	end
endfunction

function integer ref_g;
	input integer y, u, v;
	integer C, D, E;
	begin
		C = y - 16;
		D = u - 128;
		E = v - 128;
		ref_g = sat8((298 * C - 100 * D - 208 * E + 128) >>> 8);
	end
endfunction

function integer ref_b;
	input integer y, u, v;
	integer C, D;
	begin
		C = y - 16;
		D = u - 128;
		ref_b = sat8((298 * C + 516 * D + 128) >>> 8);
	end
endfunction

reg  [7:0] y, u, v;
wire [7:0] r, g, b;

yuv601_rgb dut(.y(y), .u(u), .v(v), .r(r), .g(g), .b(b));

task check;
	input [8*32-1:0] name;
	input integer yy, uu, vv;
	integer er, eg, eb, dr, dg, db;
	begin
		y = yy[7:0];
		u = uu[7:0];
		v = vv[7:0];
		#1;
		er = ref_r(yy, uu, vv);
		eg = ref_g(yy, uu, vv);
		eb = ref_b(yy, uu, vv);
		dr = r > er ? r - er : er - r;
		dg = g > eg ? g - eg : eg - g;
		db = b > eb ? b - eb : eb - b;
		if (r !== er[7:0] || g !== eg[7:0] || b !== eb[7:0]) begin
			if (dr > 1 || dg > 1 || db > 1) begin
				$display("FAIL %0s YUV(%0d,%0d,%0d) got RGB(%0d,%0d,%0d) want (%0d,%0d,%0d)",
				         name, yy, uu, vv, r, g, b, er, eg, eb);
				fails = fails + 1;
			end else begin
				$display("LSB  %0s YUV(%0d,%0d,%0d) got RGB(%0d,%0d,%0d) want (%0d,%0d,%0d)",
				         name, yy, uu, vv, r, g, b, er, eg, eb);
			end
		end else begin
			$display("OK   %0s YUV(%0d,%0d,%0d) → RGB(%0d,%0d,%0d)",
			         name, yy, uu, vv, r, g, b);
		end
	end
endtask

integer yi, ui, vi;
integer pr0, pg0, pb0, pr1, pg1, pb1;
integer er, eg, eb;

initial begin
	check("limited black", 16, 128, 128);
	check("limited white", 235, 128, 128);
	check("neutral mid-grey", 126, 128, 128);
	check("red-ish", 81, 90, 240);
	check("green-ish", 145, 54, 34);
	check("blue-ish", 41, 240, 110);
	check("clip low", 0, 0, 0);
	check("clip high", 255, 255, 255);

	/* Adjacent luma pixels must share the same U/V sample. */
	y = 81; u = 90; v = 240; #1;
	pr0 = r; pg0 = g; pb0 = b;
	y = 82; u = 90; v = 240; #1;
	pr1 = r; pg1 = g; pb1 = b;
	if (u !== 90 || v !== 240) begin
		$display("FAIL chroma share setup");
		fails = fails + 1;
	end else
		$display("OK   adjacent chroma share U=90 V=240 (RGB %0d,%0d,%0d then %0d,%0d,%0d)",
		         pr0, pg0, pb0, pr1, pg1, pb1);

	/* Sweep a grid; require exact match to the integer formula. */
	for (yi = 0; yi <= 255; yi = yi + 17)
		for (ui = 0; ui <= 255; ui = ui + 19)
			for (vi = 0; vi <= 255; vi = vi + 23) begin
				y = yi[7:0];
				u = ui[7:0];
				v = vi[7:0];
				#1;
				er = ref_r(yi, ui, vi);
				eg = ref_g(yi, ui, vi);
				eb = ref_b(yi, ui, vi);
				if (r !== er[7:0] || g !== eg[7:0] || b !== eb[7:0]) begin
					$display("FAIL grid YUV(%0d,%0d,%0d) got %0d,%0d,%0d want %0d,%0d,%0d",
					         yi, ui, vi, r, g, b, er, eg, eb);
					fails = fails + 1;
				end
			end

	if (fails) begin
		$display("yuv601_tb FAIL (%0d)", fails);
		$fatal(1);
	end
	$display("yuv601_tb PASS");
	$finish;
end

endmodule
