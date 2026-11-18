#ifndef COPPER_ENGINEERING_LOOP_H
#define COPPER_ENGINEERING_LOOP_H

#include "design_board.h"
#include "circuit.h"
#include "dfm_check.h"
#include "fab_rules.h"
#include "spice_mna.h"
#include "part_catalogue.h"
#include "placer.h"
#include "report.h"
#include "router.h"
#include "topology.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum EngineeringLoopStatus {
    ENGINEERING_LOOP_OK = 0,
    ENGINEERING_LOOP_PARTIAL,
    ENGINEERING_LOOP_REJECTED,
    ENGINEERING_LOOP_ERR_ARGUMENT,
    ENGINEERING_LOOP_ERR_MEMORY,
    ENGINEERING_LOOP_ERR_TOPOLOGY,
    ENGINEERING_LOOP_ERR_CIRCUIT,
    ENGINEERING_LOOP_ERR_SIMULATION
} EngineeringLoopStatus;

typedef enum EngineeringFindingStatus {
    ENGINEERING_FINDING_INFO = 0,
    ENGINEERING_FINDING_PASS,
    ENGINEERING_FINDING_FAIL,
    ENGINEERING_FINDING_UNVERIFIED
} EngineeringFindingStatus;

typedef struct EngineeringComponentBinding {
    const char *reference;
    const PartRecord *part; /* optional for explicit model templates */
    const char *pin_numbers[4]; /* model terminal order; required for active devices */
    size_t pin_count;
    bool has_value_override;
    double value_override_si;
    bool has_element_template;
    CircuitElement element_template; /* device/source model; nodes/reference are replaced */
} EngineeringComponentBinding;

typedef struct EngineeringLoopOptions {
    MnaOptions mna_options;
    bool run_placement_proposal;
    PlacerOptions placer_options;
    bool run_router_proposals;
    RouterOptions router_options;
    bool run_dfm;
    DfmOptions dfm_options;
    size_t dfm_finding_capacity;
    size_t finding_capacity;
    size_t max_route_proposals;
} EngineeringLoopOptions;

typedef struct EngineeringFinding {
    EngineeringFindingStatus status;
    const char *code;
    const char *reference;
    const char *net;
    const char *message;
    double measured;
    double limit;
    bool has_measurement;
} EngineeringFinding;

typedef struct EngineeringComponentStress {
    const char *reference;
    double voltage_v;
    double current_a;
    double power_w;
    bool has_voltage;
    bool has_current;
    bool has_power;
    EngineeringFindingStatus voltage_status;
    EngineeringFindingStatus current_status;
    EngineeringFindingStatus power_status;
} EngineeringComponentStress;

typedef struct EngineeringRouteProposal {
    uint32_t topology_net_index;
    const char *net_name;
    RouterStatus status;
    RouterResult route;
} EngineeringRouteProposal;

typedef struct EngineeringResolvedComponent {
    const char *reference;
    const char *part_id;
    const char *manufacturer;
    const char *mpn;
    const char *symbol_id;
    const char *footprint_id;
    const char *model_id;
    const PartRecord *part_record; /* borrowed; caller Arena must outlive serialization */
    PartKind part_kind;
    CircuitElementKind circuit_kind;
    bool has_catalogue_part;
    bool has_explicit_model;
} EngineeringResolvedComponent;

typedef struct EngineeringLoopResult {
    EngineeringLoopStatus status;
    Circuit circuit;
    bool circuit_built;
    bool simulation_attempted;
    bool simulation_converged;
    MnaStatus simulation_status;
    MnaOperatingPoint operating_point;
    uint32_t *net_nodes; /* topology net index -> circuit node (0 is ground) */
    size_t net_node_count;
    EngineeringResolvedComponent *resolved_components;
    size_t resolved_component_count;
    EngineeringComponentStress *stresses;
    size_t stress_count;
    EngineeringFinding *findings;
    size_t finding_count;
    size_t finding_capacity;
    bool placement_attempted;
    PlacerResult placement;
    bool routes_attempted;
    EngineeringRouteProposal *routes;
    size_t route_count;
    size_t route_capacity;
    bool dfm_attempted;
    CopperReport dfm_report;
    bool physical_board_supplied;
    bool design_ready;
    bool findings_truncated;
    bool parity_checked;
    size_t parity_matched_components;
    size_t parity_missing_footprints;
    size_t parity_pad_net_mismatches;
    size_t iterations; /* engineering outer-loop passes; v1 executes one deterministic pass */
} EngineeringLoopResult;

EngineeringLoopOptions engineering_loop_default_options(void);
EngineeringLoopStatus engineering_loop_run(
    Arena *arena,
    const CopperTopology *topology,
    const EngineeringComponentBinding *bindings,
    size_t binding_count,
    const Board *board,
    const BoardDiag *board_diag,
    const FabRuleSet *fab_rules,
    const EngineeringLoopOptions *options,
    EngineeringLoopResult *out);
const char *engineering_loop_status_name(EngineeringLoopStatus status);
const char *engineering_finding_status_name(EngineeringFindingStatus status);

#ifdef __cplusplus
}
#endif
#endif
