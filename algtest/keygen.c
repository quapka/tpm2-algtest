#include "keygen.h"
#include "options.h"
#include "logging.h"
#include "object_util.h"
#include "util.h"

#include <tss2/tss2_sys.h>
#include <stdlib.h>
#include <stdio.h>

extern struct tpm_algtest_options options;

/*
 * Result needs to be allocated at this point
 */
bool test_detail(
        TSS2_SYS_CONTEXT *sapi_context,
        TPMI_DH_OBJECT primary_handle,
        const struct keygen_scenario *scenario,
        struct keygen_result *result)
{
    TPM2B_PUBLIC inPublic;
    switch (scenario->type) {
    case TPM2_ALG_RSA:
        inPublic = prepare_template_RSA(scenario->keyBits);
        break;
    case TPM2_ALG_ECC:
        inPublic = prepare_template_ECC(scenario->curveID);
        break;
    default:
        log_error("Keygen: algorithm type not supported!");
        return false;
    }

    if (scenario->export_keys) {
        inPublic.publicArea.authPolicy = create_dup_policy(sapi_context);
    }

    TPM2_RC rc = test_parms(sapi_context, &inPublic.publicArea);
    if (rc != TPM2_RC_SUCCESS) {
        return false;
    }

    result->size = 0;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (unsigned i = 0; i < scenario->parameters.repetitions; ++i) {

        clock_gettime(CLOCK_MONOTONIC, &end);
        if (get_duration_s(&start, &end) > scenario->parameters.max_duration_s) {
            break;
        }

        // TODO put into separate function (keygen)
        TPM2B_PUBLIC outPublic = { .size = 0 };
        TPM2B_PRIVATE outPrivate = { .size = 0 };

        result->data_points[i].rc = create(sapi_context, &inPublic,
                primary_handle, &outPublic, &outPrivate,
                &result->data_points[i].duration_s);

        ++result->size;
        log_info("Keygen %d: type %04x | keybits %d | curve %04x | duration %f | rc %04x",
                i, scenario->type, scenario->keyBits, scenario->curveID,
                result->data_points[i].duration_s, result->data_points[i].rc);

        if (result->data_points[i].rc != TPM2_RC_SUCCESS) {
            continue;
        }

        if (scenario->export_keys && scenario->type == TPM2_ALG_RSA) {
            result->public_keys[i] = outPublic.publicArea.unique;
            TPM2_HANDLE object_handle;
            rc = load(sapi_context, primary_handle, &outPrivate, &outPublic,
                    &object_handle);
            if (rc != TPM2_RC_SUCCESS) {
                log_warning("Keygen: Cannot load object into TPM! (%04x)", rc);
                continue;
            }

            TPMU_SENSITIVE_COMPOSITE sensitive;
            rc = extract_sensitive(sapi_context, object_handle, &sensitive);
            Tss2_Sys_FlushContext(sapi_context, object_handle);
            if (rc != TPM2_RC_SUCCESS) {
                log_warning("Keygen: Cannot extract private key! (%04x)", rc);
                continue;
            } else {
                result->private_keys[i] = sensitive;
            }
        }
    }
    return true;
}

void output_results(const struct keygen_scenario *scenario,
        const struct keygen_result *result)
{
    char filename[256];
    char filename_keys[256];
    switch (scenario->type) {
    case TPM2_ALG_RSA:
        snprintf(filename, 256, "Keygen_RSA_%d.csv", scenario->keyBits);
        snprintf(filename_keys, 256, "Keygen_RSA_%d_keys.csv", scenario->keyBits);
        break;
    case TPM2_ALG_ECC:
        snprintf(filename, 256, "Keygen_ECC_0x%04x.csv", scenario->curveID);
        snprintf(filename_keys, 256, "Keygen_ECC_0x%04x_keys.csv", scenario->curveID);
        break;
    default:
        log_error("Keygen: (output_results) Algorithm type not supported.");
        return;
    }

    FILE* out = open_csv(filename, "duration,return_code");
    for (int i = 0; i < result->size; ++i) {
        struct keygen_data_point *dp = &result->data_points[i];
        fprintf(out, "%f, %04x\n", dp->duration_s, dp->rc);
    }
    fclose(out);

    if (scenario->export_keys && scenario->type == TPM2_ALG_RSA) {
        out = open_csv(filename_keys, "id;n;e;p;q;d;t");
        for (int i = 0; i < result->size; ++i) {
            if (result->data_points[i].rc != TPM2_RC_SUCCESS) {
                fprintf(out, "null;null;null;null;null;null;null\n");
                continue;
            }
            fprintf(out, "%d;", i);
            for (int j = 0; j < result->public_keys[i].rsa.size; ++j) {
                fprintf(out, "%02X", result->public_keys[i].rsa.buffer[j]);
            }
            fprintf(out, ";010001;");
            for (int j = 0; j < result->private_keys[i].rsa.size; ++j) {
                fprintf(out, "%02X", result->private_keys[i].rsa.buffer[j]);
            }
            fprintf(out, "; ; ;%d\n", (int) (result->data_points[i].duration_s * 1000));
        }
    }
}

bool alloc_result(const struct keygen_scenario *scenario,
        struct keygen_result *result)
{
    result->data_points = calloc(scenario->parameters.repetitions,
            sizeof(struct keygen_data_point));
    if (result->data_points == NULL) {
        log_error("Keygen: (calloc) Cannot allocate memory for result.");
        return false;
    }

    if (scenario->export_keys) {
        result->public_keys = calloc(scenario->parameters.repetitions,
                sizeof(TPMU_PUBLIC_ID));
        if (result->public_keys == NULL) {
            log_error("Keygen: (calloc) Cannot allocate memory for public keys.");
            free(result->data_points);
            return false;
        }
        result->private_keys = calloc(scenario->parameters.repetitions,
                sizeof(TPMU_SENSITIVE_COMPOSITE));
        if (result->private_keys == NULL) {
            log_error("Keygen: (calloc) Cannot allocate memory for public keys.");
            free(result->data_points);
            free(result->public_keys);
            return false;
        }
    }
    return true;
}

void free_result(const struct keygen_scenario *scenario,
        struct keygen_result *result)
{
    free(result->data_points);
    if (scenario->export_keys) {
        free(result->public_keys);
        free(result->private_keys);
    }
}

bool test_keygen_on_primary(TSS2_SYS_CONTEXT *sapi_context,
        const struct keygen_scenario *scenario,
        TPMI_DH_OBJECT primary_handle)
{
    struct keygen_result result;

    if (!alloc_result(scenario, &result)) {
        return false;
    }

    bool ok = test_detail(sapi_context, primary_handle, scenario, &result);
    if (ok) {
        output_results(scenario, &result);
    }

    free_result(scenario, &result);
    return ok;
}

bool create_primary_for_keygen(TSS2_SYS_CONTEXT *sapi_context,
        TPMI_DH_OBJECT *primary_handle)
{
    log_info("Keygen: creating primary...");
    TPM2_RC rc = create_some_primary(sapi_context, primary_handle);
    if (rc == TPM2_RC_SUCCESS) {
        log_info("Created primary object with handle %08x", *primary_handle);
    }
    return rc == TPM2_RC_SUCCESS;
}

bool test_keygen(TSS2_SYS_CONTEXT *sapi_context,
        const struct keygen_scenario *scenario)
{
    TPMI_DH_OBJECT primary_handle;
    bool ok = create_primary_for_keygen(sapi_context, &primary_handle);
    if (!ok) {
        return false;
    }
    // TODO deal with incomplete scenario
    ok = test_keygen_on_primary(sapi_context, scenario, primary_handle);

    Tss2_Sys_FlushContext(sapi_context, primary_handle);
    return ok;
}

void test_keygen_all(TSS2_SYS_CONTEXT *sapi_context,
        const struct scenario_parameters *parameters)
{
    struct keygen_scenario scenario = {
        .parameters = *parameters,
        .keyBits = 0,
        .curveID = 0x0000,
        .export_keys = options.export_keys,
    };

    TPMI_DH_OBJECT primary_handle;
    bool ok = create_primary_for_keygen(sapi_context, &primary_handle);
    if (!ok) {
        return;
    }

    scenario.type = TPM2_ALG_RSA;
    for (TPMI_RSA_KEY_BITS keyBits = 0; keyBits <= 2048; keyBits += 32) {
        scenario.keyBits = keyBits;
        test_keygen_on_primary(sapi_context, &scenario, primary_handle);
    }
    scenario.keyBits = 0;

    scenario.type = TPM2_ALG_ECC;
    for (TPMI_ECC_CURVE curveID = 0x0000; curveID <= 0x0020; ++curveID) {
        scenario.curveID = curveID;
        test_keygen_on_primary(sapi_context, &scenario, primary_handle);
    }

    Tss2_Sys_FlushContext(sapi_context, primary_handle);
}