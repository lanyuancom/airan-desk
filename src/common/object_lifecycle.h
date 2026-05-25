#ifndef OBJECT_LIFECYCLE_H
#define OBJECT_LIFECYCLE_H

#include "logger_manager.h"

#define DELETE_PTR_FUNC(obj_ptr) \
    if (obj_ptr)                 \
    {                            \
        obj_ptr->disconnect();   \
        delete obj_ptr;          \
        obj_ptr = nullptr;       \
    }

#define DELETELATER_PTR_FUNC(obj_ptr) \
    if (obj_ptr)                      \
    {                                 \
        obj_ptr->disconnect();        \
        obj_ptr->deleteLater();       \
        obj_ptr = nullptr;            \
    }

#define STOP_OBJ_THREAD(thread)                                                               \
    if (thread.isRunning())                                                                   \
    {                                                                                         \
        LOG_ERROR("{} thread stopping", thread.objectName());                                 \
        thread.quit();                                                                        \
        if (!thread.wait(3000))                                                               \
        {                                                                                     \
            LOG_ERROR("{} thread did not quit gracefully, terminating", thread.objectName()); \
            thread.terminate();                                                               \
            thread.wait();                                                                    \
        }                                                                                     \
        LOG_INFO("{} thread stopped", thread.objectName());                                   \
    }

#define STOP_PTR_THREAD(thread_ptr)                                                                \
    if (thread_ptr && thread_ptr->isRunning())                                                     \
    {                                                                                              \
        LOG_ERROR("{} thread stopping", thread_ptr->objectName());                                 \
        thread_ptr->quit();                                                                        \
        if (!thread_ptr->wait(3000))                                                               \
        {                                                                                          \
            LOG_ERROR("{} thread did not quit gracefully, terminating", thread_ptr->objectName()); \
            thread_ptr->terminate();                                                               \
            thread_ptr->wait();                                                                    \
        }                                                                                          \
        LOG_INFO("{} thread stopped", thread_ptr->objectName());                                   \
    }

#endif /* OBJECT_LIFECYCLE_H */
