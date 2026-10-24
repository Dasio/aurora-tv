#include "worker.h"
#include "backend/pcmanager/pclist.h"
#include "backend/pcmanager/priv.h"
#include "app.h"
#include "errors.h"

int worker_quit_app(worker_context_t *context) {
    /* gs_quit_app writes to the server, and discovery may replace (and free) node->server
     * meanwhile, so work on a copy like worker_pairing does (#86). */
    pcmanager_lock(context->manager);
    pclist_t *node = pclist_find_by_uuid(context->manager, &context->uuid);
    PSERVER_DATA server = node != NULL ? serverdata_clone(node->server) : NULL;
    pcmanager_unlock(context->manager);
    if (server == NULL) {
        return GS_ERROR;
    }
    GS_CLIENT client = app_gs_client_new(context->app);
    int ret = gs_quit_app(client, server);
    gs_destroy(client);
    if (ret == GS_OK) {
        SERVER_STATE state = {.code = SERVER_STATE_AVAILABLE};
        pclist_upsert(context->manager, &context->uuid, &state, server);
    } else {
        const char *gs_error = NULL;
        gs_get_error(&gs_error);
        context->error = gs_error != NULL ? strdup(gs_error) : NULL;
        serverdata_free(server);
    }
    return ret;
}

