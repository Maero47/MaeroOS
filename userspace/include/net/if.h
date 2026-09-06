#pragma once

#define IFNAMSIZ 16

struct ifreq {
    char ifr_name[IFNAMSIZ];
    char ifr_data[24];
};
