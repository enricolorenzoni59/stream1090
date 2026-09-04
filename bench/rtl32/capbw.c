/* IQ capture with explicit tuner bandwidth, against the vendored lib.
 * usage: capbw <dev> <freq_hz> <rate_hz> <bw_hz|0=auto> <gain_db*10> <nsamples> <out.cu8>
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "rtl-sdr.h"

int main(int argc, char** argv) {
    if (argc != 8) {
        fprintf(stderr, "usage: %s <dev> <freq> <rate> <bw> <gain*10> <nsamples> <out>\n", argv[0]);
        return 1;
    }
    int dev_i = atoi(argv[1]);
    uint32_t freq = (uint32_t)strtoul(argv[2], NULL, 10);
    uint32_t rate = (uint32_t)strtoul(argv[3], NULL, 10);
    uint32_t bw = (uint32_t)strtoul(argv[4], NULL, 10);
    int gain = atoi(argv[5]);
    uint64_t n = strtoull(argv[6], NULL, 10);
    const char* out = argv[7];

    rtlsdr_dev_t* dev = NULL;
    if (rtlsdr_open(&dev, dev_i) < 0) {
        fprintf(stderr, "open failed\n");
        return 1;
    }
    rtlsdr_set_sample_rate(dev, rate);
    rtlsdr_set_center_freq(dev, freq);
    rtlsdr_set_tuner_gain_mode(dev, 1);
    rtlsdr_set_tuner_gain(dev, gain);
    if (bw)
        rtlsdr_set_tuner_bandwidth(dev, bw);
    rtlsdr_reset_buffer(dev);

    FILE* f = fopen(out, "wb");
    if (!f) { fprintf(stderr, "open out failed\n"); return 1; }
    const size_t CHUNK = 65536;
    uint8_t* buf = malloc(CHUNK);
    uint64_t done = 0;
    int n_read = 0;
    while (done < n) {
        size_t want = CHUNK;
        if (n - done < want) want = n - done;
        if (rtlsdr_read_sync(dev, buf, want, &n_read) < 0 || n_read <= 0) break;
        fwrite(buf, 1, n_read, f);
        done += (uint64_t)n_read;
    }
    fprintf(stderr, "captured %llu of %llu samples\n", (unsigned long long)done, (unsigned long long)n);
    fclose(f);
    free(buf);
    rtlsdr_close(dev);
    return 0;
}
