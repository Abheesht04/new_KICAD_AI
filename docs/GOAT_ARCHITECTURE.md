# DFM-first hardware compiler — architecture and moat

## Core proposition

The product should not be an LLM that draws circuits.

It should be a deterministic compiler for engineered electronics:

    English intent
        -> typed requirements
        -> circuit IR
        -> electrical proof
        -> physical PCB IR
        -> manufacturing-process compilation
        -> DFM/DFA/SI/thermal/reliability proof
        -> fabrication package

The LLM is an input adapter. It is never the source of truth for engineering correctness.

## The IR hierarchy

### 1. Intent IR

Requirements, interfaces, operating ranges, mechanical envelope, environment, cost, lifecycle and manufacturing target.

Example:

    input: 5 V USB-C
    rail: 3.3 V +/- 3%
    load: 800 mA continuous
    board: 40 x 30 mm
    fab: FAB_X / process_3 / rev_12

### 2. Electrical IR

Typed components, pins, nets, constraints, models, tolerances and operating cases.

The electrical IR must be deterministic and independently testable without NLP.

### 3. Physical IR

The new `PcbBoard` layer is the beginning of this stage.

It needs to grow into:

    board outline
    stackup
    regions / keepouts
    footprints
    pads
    traces
    vias
    zones / pours
    differential pairs
    impedance structures
    drill tables
    assembly courtyards
    fiducials / test points

### 4. Manufacturing IR

A manufacturing profile should describe a real process, not a universal rule table.

    fab
    process
    revision
    stackup
    materials
    copper
    line/space
    drilling
    plating
    soldermask
    silkscreen
    surface finish
    assembly
    inspection
    panelization
    cost
    lead time

This layer is the product's long-term strategic moat.

## DFM must become a compiler stage

Do not implement:

    generate -> check -> repair manually

Implement:

    requirements + fab process
        -> constrained search space
        -> candidate layouts
        -> score candidates
        -> select Pareto-efficient candidate
        -> prove it

A DFM violation should ideally become a constraint that prevents that geometry from entering the candidate set.

## The manufacturability vector

Every candidate should eventually carry metrics such as:

    trace_width_margin
    trace_clearance_margin
    via_margin
    drill_margin
    annular_margin
    edge_margin
    copper_balance
    assembly_margin
    test_coverage
    impedance_margin
    thermal_margin
    estimated_fab_cost
    estimated_assembly_cost
    process_risk

This lets the optimizer distinguish “legal” from “good”.

## Highest-value technologies to build

### Constraint solver

Turn electrical, mechanical and manufacturing rules into a common typed constraint graph.

The solver should support hard constraints, soft constraints, priorities and tradeoff weights.

### Deterministic placement engine

Start with two-layer boards and solve placement as a constrained optimization problem:

    component geometry
    connector orientation
    power-flow proximity
    decoupling distance
    mechanical keepouts
    thermal zones
    assembly clearances
    expected routing congestion

Use deterministic seeds and reproducible candidate ranking.

### Topology-aware router

Do not start by maximizing completion rate.

Optimize for manufacturability and electrical intent:

    shortest valid route
    minimum vias
    return-path continuity
    impedance feasibility
    current capability
    manufacturability margin

### Stackup solver

Given fab capabilities plus an impedance target, solve for valid layer structures and route geometries.

A stackup must be an input to geometry synthesis, not an afterthought.

### Current / thermal sizing

Trace width should be solved from current, copper thickness, allowable temperature rise, geometry and the chosen process profile.

Keep normative standard references separate from heuristics; do not silently present an approximation as an IPC-certified calculation.

### CAM feedback model

Treat fab feedback as structured data:

    rule ID
    offending geometry
    process capability
    recommended correction
    accepted exception
    fab revision

This enables the closed loop:

    design -> CAM -> manufactured board -> inspection/test -> updated process knowledge

### Provenance / engineering audit

Every generated feature should be answerable:

    Which requirement caused this?
    Which rule constrained it?
    Which fab profile version was active?
    Which candidate layouts were rejected?
    Which solver objective selected it?
    Which simulation result proved it?

This becomes essential for aerospace, automotive, medical and industrial customers.

## Competitive positioning

### Flux

Strong at cloud-native AI-assisted electronics design, natural-language schematic generation, auto-layout, sourcing and collaboration.

Do not compete by building another chat UI around schematic capture.

### Circuit Mind / CELUS

Strong at requirements/component/schematic/BOM automation.

Beat them only by owning the physical and manufacturing compilation stages.

### Quilter

Strongest direct signal for autonomous physical PCB automation and physics-aware routing. Its 2026 manufacturing case is especially instructive: an AI-generated motherboard still accumulated 58 DFM/DFA findings during real fab review.

The response is not “more AI.” It is a deeper manufacturing model.

### Cadence

Enterprise incumbent. Treat electrical/manufacturing correctness, scale and auditability as the benchmark.

### JITX

Important conceptual competitor because it already treats requirements, stackups and manufacturing rules as code and closes the loop with simulation.

Differentiate through a much broader manufacturing-process representation and fab-native feedback loop.

## Product wedge

The first commercial product should be narrower than “design every PCB”.

Good wedge:

    English requirements
        -> deterministic 2-layer board
        -> selected fab profile
        -> DFM-constrained placement + routing
        -> complete manufacturing package
        -> machine-checkable proof report

Target boards where a measurable first-pass-build improvement matters more than exotic layout complexity.

## Success metrics

Track engineering outcomes rather than AI metrics:

    first-pass fab acceptance rate
    CAM findings per board
    ECO count after fab review
    BOM/footprint mismatch rate
    mean time from requirements to fabrication files
    percentage of geometry backed by explicit constraints
    cost delta against a human-designed baseline
    number of supported fab processes

The strongest headline metric is likely:

    first-pass manufacturing acceptance

not “percent of schematic generated by AI”.

## Machine roadmap

Once the compiler is stable, its compute profile can guide custom hardware.

Likely acceleration targets:

    sparse linear algebra
    constraint propagation
    geometric collision queries
    spatial indexes
    graph algorithms
    routing search
    branch-and-bound / candidate ranking
    parallel simulation

The right long-term machine is therefore not an “AI accelerator” in the usual sense. It is an engineering-compute system optimized for deterministic graph, geometry, sparse-matrix and search workloads.
