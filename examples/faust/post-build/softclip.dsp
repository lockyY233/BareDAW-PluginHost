import("stdfaust.lib");
drive = hslider("drive", 2, 0.1, 20, 0.1);
process = *(drive) : ma.tanh;
