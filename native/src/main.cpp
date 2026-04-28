// cad++ — desktop entry point (macOS / Windows).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Copyright (C) 2026 misut
//
// Tiny shim around the `cadpp` library archive: pulls the optional DWG
// path from argv[1], publishes it through `cadpp::g_dwg_path`, and lets
// `phenotype::native::run_app` default-construct `cadpp::State` (which
// reads the global on first call).

import std;
import phenotype;
import phenotype.native;

#include "../../src/app.hpp"

int main(int argc, char** argv) {
    if (argc >= 2 && argv[1] && argv[1][0] != '\0') {
        cadpp::g_dwg_path = argv[1];
    }
    return phenotype::native::run_app<cadpp::State, cadpp::Msg>(
        1400, 1000, "cad++", cadpp::view, cadpp::update);
}
