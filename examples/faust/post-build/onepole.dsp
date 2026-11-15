import("stdfaust.lib");
cutoff = hslider("cutoff(Hz)", 1000, 20, 18000, 1);
process = fi.lowpass(1, cutoff);
