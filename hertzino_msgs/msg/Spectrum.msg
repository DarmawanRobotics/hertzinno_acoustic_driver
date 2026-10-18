# One FFT frame from the camera (/fftData, 5 per second): the first half of the spectrum.
Header header
float32 bin_hz                # frequency of bin i = i * bin_hz; 0 while the sample rate is unknown
int16[] magnitude             # as sent by the camera (16-bit integers, unit not documented)
int32 freq_down_limit         # the analysis band at the time, kHz
int32 freq_up_limit
int32 band_peak               # largest magnitude inside the band
float32 band_peak_hz          # where it is
