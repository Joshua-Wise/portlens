#include "ui/screens.h"

screen_id_t screen_next(screen_id_t current)
{
    return (screen_id_t)((current + 1) % SCREEN_COUNT);
}
