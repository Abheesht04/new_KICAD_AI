#include "component_model.h"

const char *component_kind_name(ComponentKind kind)
{
    switch (kind) {
        case COMPONENT_GENERIC:   return "generic";
        case COMPONENT_RESISTOR:  return "resistor";
        case COMPONENT_CAPACITOR: return "capacitor";
        case COMPONENT_INDUCTOR:  return "inductor";
        case COMPONENT_DIODE:     return "diode";
        case COMPONENT_LED:       return "led";
        case COMPONENT_BJT:       return "bjt";
        case COMPONENT_MOSFET:    return "mosfet";
        case COMPONENT_CRYSTAL:   return "crystal";
        case COMPONENT_IC:        return "ic";
        case COMPONENT_CONNECTOR: return "connector";
        case COMPONENT_SOURCE:    return "source";
        case COMPONENT_CUSTOM:    return "custom";
        default:                  return "unknown";
    }
}
