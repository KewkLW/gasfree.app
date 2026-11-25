#include "clap_plugin.h"
#include <clap/clap.h>

extern "C" {

CLAP_EXPORT extern const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION_INIT,
    gasfree::gasfreeClapInit,
    gasfree::gasfreeClapDeinit,
    gasfree::gasfreeClapGetFactory
};

} // extern "C"
