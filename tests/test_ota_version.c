#include <assert.h>

#include "ota_version.h"

int main(void)
{
    assert(ota_version_is_newer("0.3.0", "0.3.1"));
    assert(!ota_version_is_newer("0.3.1", "0.3.0"));
    assert(!ota_version_is_newer("0.3.0", "0.3.0"));
    assert(ota_version_is_newer("0.3", "0.3.1"));
    assert(!ota_version_is_newer("0.3.0", "0.3"));
    assert(ota_version_is_newer("0.9.9", "0.10.0"));
    assert(!ota_version_is_newer("0.3.0-rc1", "0.3.0"));
    assert(!ota_version_is_newer("0.3.0", "0.3.0-rc1"));
    assert(ota_version_is_newer("1.0.0", "2.0.0"));
    assert(!ota_version_is_newer("2.0.0", "1.9.9"));
    return 0;
}
