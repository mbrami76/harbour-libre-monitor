// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "alerts/alerttypes.h"

namespace lm {

AlertSlot slotOf(AlertType type)
{
    switch (type) {
    case AlertType::LL:
    case AlertType::L:
        return AlertSlot::Low;
    case AlertType::H:
    case AlertType::HH:
        return AlertSlot::High;
    case AlertType::Missing:
        break;
    }
    return AlertSlot::Missing;
}

int severityOf(AlertType type)
{
    switch (type) {
    case AlertType::L:
    case AlertType::H:
    case AlertType::Missing:
        return 1;
    case AlertType::LL:
    case AlertType::HH:
        return 2;
    }
    return 0;
}

bool isUrgent(AlertType type)
{
    return type == AlertType::LL || type == AlertType::HH;
}

namespace {

struct TypeName { AlertType type; const char *name; };
const TypeName kTypes[] = {
    { AlertType::LL, "LL" }, { AlertType::L, "L" }, { AlertType::H, "H" },
    { AlertType::HH, "HH" }, { AlertType::Missing, "MISSING" }
};

struct SlotName { AlertSlot slot; const char *name; };
const SlotName kSlots[] = {
    { AlertSlot::Low, "low" }, { AlertSlot::High, "high" }, { AlertSlot::Missing, "missing" }
};

struct StateName { AlertState state; const char *name; };
const StateName kStates[] = {
    { AlertState::Normal, "normal" }, { AlertState::Pending, "pending" },
    { AlertState::Active, "active" }, { AlertState::Acknowledged, "acknowledged" },
    { AlertState::Snoozed, "snoozed" }, { AlertState::Cleared, "cleared" }
};

template <typename Table, typename Value, size_t N>
QString nameIn(const Table (&table)[N], Value value, Value Table::*member)
{
    for (size_t i = 0; i < N; ++i)
        if (table[i].*member == value)
            return QLatin1String(table[i].name);
    return QString();
}

} // namespace

QString alertTypeName(AlertType type) { return nameIn(kTypes, type, &TypeName::type); }
QString alertSlotName(AlertSlot slot) { return nameIn(kSlots, slot, &SlotName::slot); }
QString alertStateName(AlertState state) { return nameIn(kStates, state, &StateName::state); }

bool alertTypeFromName(const QString &name, AlertType *type)
{
    for (size_t i = 0; i < sizeof(kTypes) / sizeof(kTypes[0]); ++i)
        if (name == QLatin1String(kTypes[i].name)) { *type = kTypes[i].type; return true; }
    return false;
}

bool alertSlotFromName(const QString &name, AlertSlot *slot)
{
    for (size_t i = 0; i < sizeof(kSlots) / sizeof(kSlots[0]); ++i)
        if (name == QLatin1String(kSlots[i].name)) { *slot = kSlots[i].slot; return true; }
    return false;
}

bool alertStateFromName(const QString &name, AlertState *state)
{
    for (size_t i = 0; i < sizeof(kStates) / sizeof(kStates[0]); ++i)
        if (name == QLatin1String(kStates[i].name)) { *state = kStates[i].state; return true; }
    return false;
}

} // namespace lm
