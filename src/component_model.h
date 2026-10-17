#ifndef COMPONENT_MODEL_H
#define COMPONENT_MODEL_H

#include "range.h"

#include <stddef.h>

struct DiagnosticList;
struct ConstraintList;
typedef struct {
  double voltage_dc_v;
} SourceModel;
/*
 * ComponentKind is intentionally coarse-grained.
 * Device-specific behavior is carried by ComponentModelData and ModelOps,
 * not by growing this enum for every manufacturer/device variant.
 */
typedef enum {
  COMPONENT_GENERIC,
  COMPONENT_RESISTOR,
  COMPONENT_CAPACITOR,
  COMPONENT_INDUCTOR,
  COMPONENT_DIODE,
  COMPONENT_LED,
  COMPONENT_BJT,
  COMPONENT_MOSFET,
  COMPONENT_CRYSTAL,
  COMPONENT_IC,
  COMPONENT_CONNECTOR,
  COMPONENT_SOURCE,
  COMPONENT_CUSTOM
} ComponentKind;

typedef struct {
  Range resistance_ohm;
} ResistorModel;

typedef struct {
  Range capacitance_f;
  Range esr_ohm;
  double voltage_rating_v;
} CapacitorModel;

typedef struct {
  Range inductance_h;
  Range esr_ohm;
  double saturation_current_a;
} InductorModel;

typedef struct {
  Range forward_voltage_v;
  double reverse_voltage_v;
  double saturation_current_a;
  double ideality_factor;
} DiodeModel;

typedef struct {
  Range threshold_voltage_v;
  Range rds_on_ohm;
  double max_vds_v;
  double max_id_a;
} MosfetModel;

typedef struct {
  Range frequency_hz;
  double load_capacitance_f;
} CrystalModel;

typedef union {
  ResistorModel resistor;
  CapacitorModel capacitor;
  InductorModel inductor;
  DiodeModel diode;
  MosfetModel mosfet;
  CrystalModel crystal;
  SourceModel source;
} ComponentModelData;

typedef struct {
  ComponentKind kind;
  ComponentModelData data;
} ComponentModel;

const char *component_kind_name(ComponentKind kind);

#endif
