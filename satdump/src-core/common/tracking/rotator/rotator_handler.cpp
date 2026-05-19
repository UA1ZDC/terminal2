#include "rotator_handler.h"
#include "core/plugin.h"

#include "rotcl_handler.h"
#include "anglespeed_handler.h"

namespace rotator
{
    std::vector<RotatorHandlerOption> getRotatorHandlerOptions()
    {
        std::vector<RotatorHandlerOption> rotatorOptions;

        rotatorOptions.push_back({"anglespeed", []()
                                  { return std::make_shared<AngleSpeedHandler>(); }});

        rotatorOptions.push_back({"rotctl", []()
                                  { return std::make_shared<RotctlHandler>(); }});

        satdump::eventBus->fire_event<RequestRotatorHandlerOptionsEvent>({rotatorOptions});

        return rotatorOptions;
    }
}