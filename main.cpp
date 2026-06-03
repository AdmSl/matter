/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */
#include <app/server/Server.h>
#include <setup_payload/SetupPayload.h>

#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <zephyr/modbus/modbus.h>
#include <zephyr/drivers/uart.h>

#include <platform/ConfigurationManager.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/ThreadStackManager.h>
#include <platform/ConnectivityManager.h>
#include <lib/support/Span.h>
#include <zephyr/net/openthread.h>
#include <openthread/dataset.h>
#include <openthread/thread.h>
#include <openthread/instance.h>

#include <app/ConcreteCommandPath.h>
#include <app/CommandHandler.h>
// Matter includes
#include "app_task.h"
#include <app/clusters/on-off-server/on-off-server.h>
#include <app-common/zap-generated/attributes/Accessors.h>
#include <app/ConcreteAttributePath.h>
#include <app/util/attribute-storage.h>

#include <platform/CHIPDeviceLayer.h>

#include <app/util/endpoint-config-api.h>
#include <app/clusters/on-off-server/on-off-server.h>

#include <bridged_device_types/onoff_light.h>
#include <bridged_device_types/onoff_light_switch.h>
#include "core/bridge_manager.h"

#include "simulated_providers/simulated_onoff_light_data_provider.h"

using namespace chip::app::Clusters;

LOG_MODULE_REGISTER(app, CONFIG_CHIP_APP_LOG_LEVEL);

K_THREAD_STACK_DEFINE(modbus_stack_area, 2048);
struct k_thread modbus_thread_data;

static int client_iface = -1; 
static const char iface_name[] = "modbus0";

// extern "C" void MatterPostAttributeChangeCallback(const chip::app::ConcreteAttributePath & attributePath,
//                                        uint8_t type, uint16_t size, uint8_t * value)
// {
//     // Sprawdzamy: Endpoint 2 (nasza lampa) i Klaster OnOff
//     if (attributePath.mEndpointId == 2 && attributePath.mClusterId == OnOff::Id) 
//     {
//         if (attributePath.mAttributeId == OnOff::Attributes::OnOff::Id) 
//         {
//             bool newValue = *value;

//             if (client_iface >= 0) {
//                 int err = modbus_write_coil(client_iface, 0x01, 0x00, newValue);

//                 if (err == 0) {
//                     printk("Matter -> Modbus: SUCCESS - Set Relay to %s\n", newValue ? "ON" : "OFF");
//                 } else {
//                     printk("Matter -> Modbus: ERROR %d - Fail to write coil\n", err);
//                 }
//             }
//         }
//     }
// }
extern "C" void MatterPostAttributeChangeCallback(const chip::app::ConcreteAttributePath & attributePath,
                                       uint8_t type, uint16_t size, uint8_t * value)
{
    if (attributePath.mEndpointId == 2 && attributePath.mClusterId == OnOff::Id) 
    {
        if (attributePath.mAttributeId == OnOff::Attributes::OnOff::Id) 
        {
            bool newValue = *value;

            // [TUTAJ] Możesz dopisać obsługę LED z zakomentowanego kodu, jeśli chcesz:
            // Nrf::GetBoard().GetLED(Nrf::DeviceLeds::LED2).Set(newValue);

            // Twoja obecna logika Modbus
            if (client_iface >= 0) {
                int err = modbus_write_coil(client_iface, 0x01, 0x00, newValue);
                // ... reszta logiki logowania ...
            }
        }
    }
}

// Synchronizacja z Modbus do Matter
void UpdateMatterStatus(bool isOn) {
    const uint16_t targetEndpoint = 3; // ZMIANA: Stan wysyłamy na Endpoint 3

    chip::app::Clusters::OnOff::Attributes::OnOff::Set(targetEndpoint, isOn);
    
    printk("Modbus -> Matter: Sync Endpoint %d to %s\n", 
            targetEndpoint, isOn ? "ON" : "OFF");
}
//sync modbus x matter
// void UpdateMatterStatus(bool isOn) {
//     const uint16_t targetEndpoint = 2;

//     chip::app::Clusters::OnOff::Attributes::OnOff::Set(targetEndpoint, isOn);
    
//     printk("Modbus -> Matter: Sync Endpoint %d to %s\n", 
//             targetEndpoint, isOn ? "ON" : "OFF");
// }

// void UpdateMatterStatus(bool isOn) {
//     if (modbusLight != nullptr) {
//         // Klasy Nordica zazwyczaj mają metodę SetOnOff lub SetState
//         modbusLight->SetOnOff(isOn); 
        
//         printk("Modbus -> Matter: Zsynchronizowano stan na %s\n", isOn ? "ON" : "OFF");
//     }
// }

void modbus_thread_entry(void *, void *, void *)
{
    //czytanie if modbusa
    client_iface = modbus_iface_get_by_name(iface_name);

    struct modbus_iface_param param = {
        .mode = MODBUS_MODE_RTU,
        .rx_timeout = 500000,
        .serial = {
            .baud = 9600,
            .parity = UART_CFG_PARITY_NONE,
            .stop_bits = UART_CFG_STOP_BITS_1,
        }
    };

    if (client_iface < 0 || modbus_init_client(client_iface, param) != 0) {
        LOG_ERR("Modbus: Inicjalizacja nieudana! Sprawdz app.overlay.");
        return;
    }

    LOG_INF("Modbus: Interfejs zainicjalizowany.");

    bool lastKnownState = false;
    int check_counter = 0;

    while (1) {
        if (check_counter++ % 10 == 0) {
            otInstance *inst = openthread_get_default_instance();
            if (inst) {
                otDeviceRole role = otThreadGetDeviceRole(inst);
                const char *role_str = "UNKNOWN";
                
                switch (role) {
                    case OT_DEVICE_ROLE_DISABLED: role_str = "DISABLED"; break;
                    case OT_DEVICE_ROLE_DETACHED: role_str = "DETACHED (Szukam dongla...)"; break;
                    case OT_DEVICE_ROLE_CHILD:    role_str = "CONNECTED (Child)"; break;
                    case OT_DEVICE_ROLE_ROUTER:   role_str = "CONNECTED (Router)"; break;
                    case OT_DEVICE_ROLE_LEADER:   role_str = "CONNECTED (Leader)"; break;
                }
                LOG_INF("--- THREAD STATUS: %s ---", role_str);
            }
        }
        uint8_t coil_val = 0;
        
        //cewka 0 adres 1
        int err = modbus_read_coils(client_iface, 0x01, 0x00, &coil_val, 1);

        if (err == 0) {
            bool currentState = (coil_val != 0);
            
            if (currentState != lastKnownState) {
                UpdateMatterStatus(currentState);
                lastKnownState = currentState;
            }
        } else {
            static int error_counter = 0;
            if (error_counter++ % 10 == 0) {
                LOG_WRN("Modbus: Brak odpowiedzi (Error %d) - sprawdz okablowanie.", err);
            }
        }

        k_msleep(1000);
    }
}
void ForceThreadConnection() {
    struct otInstance *ot_instance = openthread_get_default_instance();

    if (ot_instance == NULL) {
        printk("BŁĄD: Nie można pobrać instancji OpenThread!\n");
        return;
    }


    //hex zamieniony na bajty
    uint8_t datasetRaw[] = {
        0x0e, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
        0x00, 0x03, 0x00, 0x00, 0x18, 0x4a, 0x03, 0x00, 0x00, 0x16,
        0x35, 0x06, 0x00, 0x04, 0x00, 0x1f, 0xff, 0xe0, 0x02, 0x08,
        0x13, 0xe8, 0x53, 0x55, 0x88, 0xb4, 0xe5, 0x57, 0x70, 0x08,
        0xfd, 0xac, 0x63, 0x5f, 0xab, 0x31, 0xe8, 0x18, 0x05, 0x10,
        0x09, 0x93, 0x43, 0xa4, 0x7e, 0xda, 0x8b, 0x90, 0x72, 0xf8,
        0xfe, 0x64, 0x33, 0xa4, 0x31, 0xe3, 0x03, 0x0f, 0x4f, 0x70,
        0x65, 0x6e, 0x54, 0x68, 0x72, 0x65, 0x61, 0x64, 0x2d, 0x30,
        0x33, 0x35, 0x61, 0x01, 0x02, 0x03, 0x5a, 0x04, 0x10, 0x5b,
        0x68, 0x2a, 0xb4, 0xfd, 0x32, 0xa2, 0x4f, 0x39, 0x50, 0x41,
        0xd7, 0xc3, 0x61, 0xd2, 0xfe, 0x0c, 0x04, 0x02, 0xa0, 0xf7,
        0xf8
        };

    //struktura datasetu w threadzie
    otOperationalDatasetTlvs datasetTlvs;
    datasetTlvs.mLength = sizeof(datasetRaw);
    memcpy(datasetTlvs.mTlvs, datasetRaw, sizeof(datasetRaw));

    //force threada
    otError error = otDatasetSetActiveTlvs(ot_instance, &datasetTlvs);

    if (error == OT_ERROR_NONE) {
        otIp6SetEnabled(ot_instance, true);
        otThreadSetEnabled(ot_instance, true);
        printk("SUCCESS: Dataset wgrany bezpośrednio do OpenThread!\n");
    } else {
        printk("BŁĄD: Nie udało się ustawić datasetu (Error: %d)\n", error);
    }
}
// --- 1. Definicje wymaganych klastrów systemowych ---
// Klaster: Bridged Device Basic Information (wymagany dla urządzeń za mostkiem)
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(bridgedDeviceBasicAttrs)
    DECLARE_DYNAMIC_ATTRIBUTE(chip::app::Clusters::BridgedDeviceBasicInformation::Attributes::ClusterRevision::Id, INT16U, 2, 0),
DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

// Klaster: Descriptor (mówi chip-toolowi jakie klastry są na tym endpoincie)
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(descriptorAttrs)
    DECLARE_DYNAMIC_ATTRIBUTE(chip::app::Clusters::Descriptor::Attributes::ClusterRevision::Id, INT16U, 2, 0),
DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

// Klaster: Twój Modbus On/Off Server
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(modbusOnOffAttrs)
    DECLARE_DYNAMIC_ATTRIBUTE(chip::app::Clusters::OnOff::Attributes::OnOff::Id, BOOLEAN, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(chip::app::Clusters::OnOff::Attributes::ClusterRevision::Id, INT16U, 2, 0),
DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();


// --- 2. Lista klastrów przypisana do Endpointu ---
DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(modbusClusters)
    DECLARE_DYNAMIC_CLUSTER(chip::app::Clusters::Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(chip::app::Clusters::BridgedDeviceBasicInformation::Id, bridgedDeviceBasicAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(chip::app::Clusters::OnOff::Id, modbusOnOffAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
DECLARE_DYNAMIC_CLUSTER_LIST_END;

// --- 3. Definicja samego Endpointu ---
DECLARE_DYNAMIC_ENDPOINT(modbusEndpoint, modbusClusters);

// --- 4. Typ urządzenia (0x0100 = On/Off Light) ---
static const chip::app::DataModel::DeviceTypeEntry modbusDeviceTypeList[] = { 
    { 0x0100, 1 }, // On/Off Light (Device ID: 0x0100, Version: 1)
    { 0x0013, 1 }  // Bridged Node (Device ID: 0x0013, Version: 1)
};

static chip::DataVersion modbusDataVersions[MATTER_ARRAY_SIZE(modbusClusters)];


void RegisterMyModbusEndpoint() {
    chip::Span<chip::DataVersion> dataVersionSpan(modbusDataVersions);
    chip::Span<const chip::app::DataModel::DeviceTypeEntry> deviceTypeSpan(modbusDeviceTypeList);
    
    // Rejestracja w systemie Matter jako Endpoint 2
    CHIP_ERROR err = emberAfSetDynamicEndpoint(
        0,                           // Indeks w tablicy (0 dla pierwszego dynamicznego urządzenia)
        2,                           // Żądany ID Endpointu w sieci Matter
        &modbusEndpoint,             // Wskaźnik na naszą strukturę klastrów
        dataVersionSpan, 
        deviceTypeSpan, 
        1                            // Parent ID (Endpoint 0)
    );
    
    if (err == CHIP_NO_ERROR) {
        printk("SUCCESS: Dynamic Endpoint 2 registered successfully!\n");
    } else {
        printk("ERROR: Failed to register Dynamic Endpoint 2 (Code: %" CHIP_ERROR_FORMAT ")\n", err.Format());
    }
}

int main()
{
    k_thread_create(&modbus_thread_data, modbus_stack_area,
                    K_THREAD_STACK_SIZEOF(modbus_stack_area),
                    modbus_thread_entry, NULL, NULL, NULL,
                    K_PRIO_PREEMPT(10), 0, K_NO_WAIT);

    bool isCommissioned = chip::DeviceLayer::ConfigurationMgr().IsFullyProvisioned();

    if (!isCommissioned) {
        printk("Urządzenie nieodparowane - wymuszam Thread dla parowania...\n");
        ForceThreadConnection();
    } else {
        printk("Urządzenie już sparowane - Matter sam zajmie się siecią.\n");
        // Tutaj nie wywołujemy ForceThreadConnection, 
        // Matter sam przywróci połączenie z pamięci NVS.
    }
    CHIP_ERROR err = AppTask::Instance().StartApp();
    if (err != CHIP_NO_ERROR) {
        LOG_ERR("Matter App failed to start: %" CHIP_ERROR_FORMAT, err.Format());
    }
    else {
        // DODAJ TO TUTAJ:
        // Otwieramy okno na 15 minut (900 sekund)
        // Robimy to po StartApp, kiedy serwer już powinien wstawać
        chip::DeviceLayer::PlatformMgr().ScheduleWork([](intptr_t) {
            RegisterMyModbusEndpoint();
            chip::Server::GetInstance().GetCommissioningWindowManager().OpenBasicCommissioningWindow(
                chip::System::Clock::Seconds16(900)
            );
            printk("Matter Commissioning Window OPENED (900s)\n");
        });
    }
    return (err == CHIP_NO_ERROR) ? EXIT_SUCCESS : EXIT_FAILURE;
}
