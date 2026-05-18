#ifndef SHADPS4_ELISA_VIDEOOUT_VALIDATION_H
#define SHADPS4_ELISA_VIDEOOUT_VALIDATION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int64_t shadps4_elisa_videoout_validate_register_shape(int64_t start_index, int64_t buffer_num,
                                                       intptr_t has_attribute,
                                                       intptr_t has_addresses);
int64_t shadps4_elisa_videoout_validate_attribute(uint32_t pixel_format, int64_t tiling_mode,
                                                  int64_t aspect_ratio, uint64_t width,
                                                  uint64_t height, uint64_t pitch_in_pixel,
                                                  uint64_t reserved0, uint64_t reserved1);
int64_t shadps4_elisa_videoout_validate_flip_rate(int64_t rate);
int64_t shadps4_elisa_videoout_validate_buffer_index(int64_t buf_id, intptr_t registered);
intptr_t shadps4_elisa_videoout_buffer_guest_size(uint32_t pixel_format, uint64_t width,
                                                  uint64_t height, uint64_t pitch_in_pixel,
                                                  uint64_t* out_size);

#ifdef __cplusplus
}
#endif

#endif
