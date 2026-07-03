#ifndef BMI270_CONFIG_H
#define BMI270_CONFIG_H

#include <stdint.h>

// Reuse the complete Bosch BMI270 firmware image bundled by the official
// espressif/bmi270_sensor component. Rename the symbol locally to avoid
// collisions with the prebuilt component library.
#define bmi270_config_file lmcl_bmi270_config_file
#include "../../managed_components/espressif__bmi270_sensor/firmware/bmi270_image.h"
#undef bmi270_config_file

#endif // BMI270_CONFIG_H
