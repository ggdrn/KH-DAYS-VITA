#include "msgdialog.h"

#include <psp2/common_dialog.h>
#include <psp2/message_dialog.h>
#include <string.h>
#include <vitaGL.h>

void msgdialog_show(const char *text)
{
    SceMsgDialogParam param;
    SceMsgDialogUserMessageParam msg;

    sceMsgDialogParamInit(&param);
    memset(&msg, 0, sizeof(msg));
    msg.msg = (const SceChar8 *)text;
    msg.buttonType = SCE_MSG_DIALOG_BUTTON_TYPE_OK;
    param.mode = SCE_MSG_DIALOG_MODE_USER_MSG;
    param.userMsgParam = &msg;
    if (sceMsgDialogInit(&param) < 0)
        return;

    while (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) {
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        vglSwapBuffers(GL_TRUE); /* GL_TRUE: let the common dialog draw over the frame */
    }
    sceMsgDialogTerm();
}
