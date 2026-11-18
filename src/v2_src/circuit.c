#include "circuit.h"
#include "nmath.h"

#include <math.h>
#include <string.h>

#if COPPER_TRACE
static CopperTraceCallback g_trace_callback;
static void *g_trace_context;
#endif

void copper_trace_set_callback(CopperTraceCallback callback, void *context)
{
#if COPPER_TRACE
    g_trace_callback = callback;
    g_trace_context = context;
#else
    (void)callback;
    (void)context;
#endif
}

void copper_trace_emit(const CopperTraceEvent *event)
{
#if COPPER_TRACE
    if (event && g_trace_callback)
        g_trace_callback(g_trace_context, event);
#else
    (void)event;
#endif
}

static bool valid_node(const Circuit *c, uint32_t n)
{
    return c != NULL && n <= c->node_count;
}

static bool finite_positive(double x) { return nm_isfinite(x) && x > 0.0; }

const char *circuit_status_name(CircuitStatus status)
{
    static const char *const names[] = {
        "OK", "ARGUMENT_ERROR", "MEMORY_ERROR", "RANGE_ERROR", "UNSUPPORTED"
    };
    return (unsigned)status < sizeof(names) / sizeof(names[0])
        ? names[status] : "UNKNOWN";
}

const char *circuit_element_kind_name(CircuitElementKind kind)
{
    static const char *const names[] = {
        "INVALID", "RESISTOR", "CAPACITOR", "INDUCTOR",
        "VOLTAGE_SOURCE", "CURRENT_SOURCE", "DIODE", "BJT", "MOSFET",
        "JFET", "VCVS", "VCCS", "CCCS", "CCVS"
    };
    return (unsigned)kind < sizeof(names) / sizeof(names[0])
        ? names[kind] : "UNKNOWN";
}

CircuitStatus circuit_init(Arena *arena, Circuit *out,
                           uint32_t node_count, size_t element_capacity)
{
    if (!arena || !out || node_count == 0 || element_capacity == 0)
        return CIRCUIT_ERR_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->elements = (CircuitElement *)arena_alloc_array(
        arena, sizeof(CircuitElement), element_capacity);
    if (!out->elements) return CIRCUIT_ERR_MEMORY;
    memset(out->elements, 0, sizeof(CircuitElement) * element_capacity);
    out->node_count = node_count;
    out->element_capacity = element_capacity;
    return CIRCUIT_OK;
}

static bool source_waveform_valid(const CircuitElement *e)
{
    if (e->waveform.kind == CIRCUIT_WAVEFORM_NONE) return true;
    size_t count = e->waveform.kind == CIRCUIT_WAVEFORM_PULSE ? 7u :
                   e->waveform.kind == CIRCUIT_WAVEFORM_SIN ? 6u :
                   e->waveform.kind == CIRCUIT_WAVEFORM_EXP ? 6u : 0u;
    if (!count) return false;
    for (size_t i = 0; i < count; ++i)
        if (!nm_isfinite(e->waveform.p[i])) return false;
    if (e->waveform.kind == CIRCUIT_WAVEFORM_PULSE)
        return e->waveform.p[3] >= 0.0 && e->waveform.p[4] >= 0.0 &&
               e->waveform.p[5] >= 0.0 && e->waveform.p[6] >= 0.0;
    if (e->waveform.kind == CIRCUIT_WAVEFORM_SIN)
        return e->waveform.p[2] >= 0.0 && e->waveform.p[3] >= 0.0 &&
               e->waveform.p[4] >= 0.0;
    return e->waveform.p[3] > 0.0 && e->waveform.p[5] > 0.0;
}

static bool element_valid(const Circuit *c, const CircuitElement *e)
{
    if (!c || !e || e->kind <= CIRCUIT_ELEMENT_INVALID ||
        e->kind > CIRCUIT_CCVS) return false;
    if (!valid_node(c, e->n1) || !valid_node(c, e->n2) || e->n1 == e->n2)
        return false;
    if (!nm_isfinite(e->value_si)) return false;

    switch (e->kind) {
    case CIRCUIT_RESISTOR:
        return e->value_si > 0.0;
    case CIRCUIT_CAPACITOR:
    case CIRCUIT_INDUCTOR:
        return e->value_si >= 0.0;
    case CIRCUIT_VOLTAGE_SOURCE:
    case CIRCUIT_CURRENT_SOURCE:
        return cplx_isfinite(e->ac_value) && source_waveform_valid(e);
    case CIRCUIT_DIODE:
        return finite_positive(e->model.diode.saturation_current_a) &&
               finite_positive(e->model.diode.emission_coefficient) &&
               finite_positive(e->model.diode.thermal_voltage_v) &&
               nm_isfinite(e->model.diode.series_resistance_ohm) &&
               e->model.diode.series_resistance_ohm >= 0.0 &&
               nm_isfinite(e->model.diode.breakdown_voltage_v) &&
               e->model.diode.breakdown_voltage_v >= 0.0 &&
               nm_isfinite(e->model.diode.breakdown_current_a) &&
               e->model.diode.breakdown_current_a >= 0.0 &&
               nm_isfinite(e->model.diode.junction_capacitance_f) &&
               e->model.diode.junction_capacitance_f >= 0.0 &&
               nm_isfinite(e->model.diode.junction_potential_v) &&
               (e->model.diode.junction_capacitance_f == 0.0 ||
                e->model.diode.junction_potential_v > 0.0) &&
               nm_isfinite(e->model.diode.grading_coefficient) &&
               e->model.diode.grading_coefficient >= 0.0 &&
               e->model.diode.grading_coefficient < 1.0 &&
               nm_isfinite(e->model.diode.transit_time_s) &&
               e->model.diode.transit_time_s >= 0.0;
    case CIRCUIT_BJT:
        return valid_node(c, e->n3) && e->n1 != e->n3 && e->n2 != e->n3 &&
               e->n2 != e->n1 && finite_positive(e->model.bjt.saturation_current_a) &&
               finite_positive(e->model.bjt.beta_forward) &&
               nm_isfinite(e->model.bjt.beta_reverse) &&
               nm_isfinite(e->model.bjt.early_voltage_v) &&
               finite_positive(e->model.bjt.thermal_voltage_v) &&
               e->model.bjt.beta_reverse >= 0.0 && e->model.bjt.early_voltage_v >= 0.0 &&
               nm_isfinite(e->model.bjt.cje_f) && e->model.bjt.cje_f >= 0.0 &&
               nm_isfinite(e->model.bjt.cjc_f) && e->model.bjt.cjc_f >= 0.0 &&
               nm_isfinite(e->model.bjt.vje_v) &&
               (e->model.bjt.cje_f == 0.0 || e->model.bjt.vje_v > 0.0) &&
               nm_isfinite(e->model.bjt.vjc_v) &&
               (e->model.bjt.cjc_f == 0.0 || e->model.bjt.vjc_v > 0.0) &&
               nm_isfinite(e->model.bjt.mje) && e->model.bjt.mje >= 0.0 &&
               e->model.bjt.mje < 1.0 && nm_isfinite(e->model.bjt.mjc) &&
               e->model.bjt.mjc >= 0.0 && e->model.bjt.mjc < 1.0 &&
               nm_isfinite(e->model.bjt.tf_s) && e->model.bjt.tf_s >= 0.0 &&
               (e->model.bjt.polarity == CIRCUIT_BJT_NPN ||
                e->model.bjt.polarity == CIRCUIT_BJT_PNP);
    case CIRCUIT_MOSFET:
        return valid_node(c, e->n3) && valid_node(c, e->n4) &&
               e->n1 != e->n3 && e->n2 != e->n3 &&
               finite_positive(e->model.mos.kp_a_per_v2) &&
               nm_isfinite(e->model.mos.threshold_voltage_v) &&
               nm_isfinite(e->model.mos.channel_modulation_per_v) &&
               e->model.mos.channel_modulation_per_v >= 0.0 &&
               nm_isfinite(e->model.mos.body_effect_gamma) &&
               e->model.mos.body_effect_gamma >= 0.0 &&
               nm_isfinite(e->model.mos.surface_potential_v) &&
               e->model.mos.surface_potential_v >= 0.0 &&
               nm_isfinite(e->model.mos.gate_oxide_capacitance_f) &&
               e->model.mos.gate_oxide_capacitance_f >= 0.0 &&
               nm_isfinite(e->model.mos.gate_overlap_gs_f) &&
               e->model.mos.gate_overlap_gs_f >= 0.0 &&
               nm_isfinite(e->model.mos.gate_overlap_gd_f) &&
               e->model.mos.gate_overlap_gd_f >= 0.0 &&
               nm_isfinite(e->model.mos.gate_overlap_gb_f) &&
               e->model.mos.gate_overlap_gb_f >= 0.0 &&
               (e->model.mos.polarity == CIRCUIT_MOS_N ||
                e->model.mos.polarity == CIRCUIT_MOS_P);
    case CIRCUIT_JFET:
        return valid_node(c, e->n3) && e->n1 != e->n3 && e->n2 != e->n3 &&
               finite_positive(e->model.jfet.idss_a) &&
               nm_isfinite(e->model.jfet.pinch_off_voltage_v) &&
               e->model.jfet.pinch_off_voltage_v != 0.0 &&
               nm_isfinite(e->model.jfet.channel_modulation_per_v) &&
               e->model.jfet.channel_modulation_per_v >= 0.0 &&
               (e->model.jfet.polarity == CIRCUIT_JFET_N ||
                e->model.jfet.polarity == CIRCUIT_JFET_P);
    case CIRCUIT_VCVS:
    case CIRCUIT_VCCS:
        return valid_node(c, e->n3) && valid_node(c, e->n4) && e->n3 != e->n4;
    case CIRCUIT_CCCS:
    case CIRCUIT_CCVS:
        return true; /* control reference validated once the complete circuit exists */
    default:
        return false;
    }
}

CircuitStatus circuit_add(Circuit *circuit, const CircuitElement *element)
{
    if (!circuit || !element) return CIRCUIT_ERR_ARGUMENT;
    if (circuit->element_count >= circuit->element_capacity)
        return CIRCUIT_ERR_RANGE;
    if (!element_valid(circuit, element)) return CIRCUIT_ERR_RANGE;
    circuit->elements[circuit->element_count++] = *element;
    return CIRCUIT_OK;
}

CircuitStatus circuit_validate(const Circuit *circuit)
{
    if (!circuit || circuit->node_count == 0 ||
        (circuit->element_count && !circuit->elements))
        return CIRCUIT_ERR_ARGUMENT;
    if (circuit->element_count > circuit->element_capacity)
        return CIRCUIT_ERR_RANGE;
    for (size_t i = 0; i < circuit->element_count; ++i) {
        const CircuitElement *e = &circuit->elements[i];
        if (!element_valid(circuit, e)) return CIRCUIT_ERR_RANGE;
        if (e->kind == CIRCUIT_CCCS || e->kind == CIRCUIT_CCVS) {
            if (e->control_element_index >= circuit->element_count ||
                e->control_element_index == i) return CIRCUIT_ERR_RANGE;
            CircuitElementKind ck = circuit->elements[e->control_element_index].kind;
            if (ck != CIRCUIT_VOLTAGE_SOURCE && ck != CIRCUIT_VCVS &&
                ck != CIRCUIT_CCVS && ck != CIRCUIT_INDUCTOR)
                return CIRCUIT_ERR_RANGE;
        }
    }
    return CIRCUIT_OK;
}

double circuit_source_value(const CircuitElement *e, double t)
{
    if (!e || !nm_isfinite(t)) return 0.0;
    const double *p = e->waveform.p;
    switch (e->waveform.kind) {
    case CIRCUIT_WAVEFORM_PULSE: {
        double v1=p[0],v2=p[1],td=p[2],tr=p[3],tf=p[4],pw=p[5],per=p[6];
        if (t < td) return v1;
        double tau = per > 0.0 ? fmod(t-td,per) : t-td;
        if (tr > 0.0 && tau < tr) return v1+(v2-v1)*(tau/tr);
        tau -= tr;
        if (tau < pw) return v2;
        tau -= pw;
        if (tf > 0.0 && tau < tf) return v2+(v1-v2)*(tau/tf);
        return v1;
    }
    case CIRCUIT_WAVEFORM_SIN: {
        double offset=p[0],amp=p[1],freq=p[2],delay=p[3],damp=p[4],phase=p[5];
        if (t < delay) return offset;
        double tau=t-delay;
        return offset+amp*exp(-damp*tau)*sin(6.2831853071795864769*freq*tau+phase);
    }
    case CIRCUIT_WAVEFORM_EXP: {
        double v1=p[0],v2=p[1],td1=p[2],tau1=p[3],td2=p[4],tau2=p[5];
        double value=v1;
        if(t>=td1)value+=(v2-v1)*(1.0-exp(-(t-td1)/tau1));
        if(t>=td2)value-=(v2-v1)*(1.0-exp(-(t-td2)/tau2));
        return value;
    }
    default: return e->value_si;
    }
}


double circuit_source_dc_value(const CircuitElement *e)
{
    if(!e)return 0.0;
    if(e->waveform.kind==CIRCUIT_WAVEFORM_NONE||e->has_dc_value)return e->value_si;
    switch(e->waveform.kind){
    case CIRCUIT_WAVEFORM_PULSE:
    case CIRCUIT_WAVEFORM_SIN:
    case CIRCUIT_WAVEFORM_EXP:return e->waveform.p[0];
    default:return e->value_si;
    }
}
