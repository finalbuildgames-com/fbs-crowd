#include <fbs/crowd.h>
int main(void) {
    fbs_crowd_config config = {8, 1, 1, 8, 2, 1, 1};
    fbs_crowd *context = 0;
    if (fbs_crowd_create(&config, &context) != FBS_CROWD_OK) return 1;
    fbs_crowd_destroy(context);
    return 0;
}
