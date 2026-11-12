#pragma once

// PortableManager Desktop owns the presentation layer: pill, desktop,
// menus and windows. PMD can call PML core APIs. Core and guard mode
// have no dependency on PMD. UI integration is reserved for a later phase.
namespace pm::pmd {}
