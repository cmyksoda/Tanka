#pragma once
#include <SupportDefs.h>
off_t fat32_find_contiguous_file(const char* path, off_t* out_size);
