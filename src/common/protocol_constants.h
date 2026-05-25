#ifndef PROTOCOL_CONSTANTS_H
#define PROTOCOL_CONSTANTS_H

#include <QString>

namespace Constant
{
    static const QString MSG_TYPE_BINARY = "binary";
    static const QString MSG_TYPE_TEXT = "text";
    static const QString MSG_TYPE_OTHER = "other";

    static const QString KEY_NAME = "name";
    static const QString KEY_STATUS = "status";
    static const QString KEY_NEW_SESSION_ID = "newSessionId";
    static const QString KEY_ROLE = "role";
    static const QString KEY_SENDER = "sender";
    static const QString KEY_RECEIVER = "receiver";
    static const QString KEY_TYPE = "type";
    static const QString KEY_DATA = "data";
    static const QString KEY_MID = "mid";
    static const QString KEY_HEIGHT = "height";
    static const QString KEY_WIDTH = "width";
    static const QString KEY_FPS = "fps";
    static const QString KEY_LABEL_NAME = "label_name";
    static const QString KEY_IS_ONLY_FILE = "is_only_file";
    static const QString KEY_ONLY_RELAY = "only_relay";

    static const QString KEY_FILE_PATH = "file_path";
    static const QString KEY_FILE_DATA = "file_data";
    static const QString KEY_FILE_NUM = "file_num";
    static const QString KEY_FILE_NOW_NUM = "file_now_num";
    static const QString KEY_FILE_SIZE = "file_size";
    static const QString KEY_TRANSFER_ID = "transferId";
    static const QString KEY_TRANSFER_BYTES = "transferBytes";
    static const QString KEY_TRANSFER_TOTAL_BYTES = "transferTotalBytes";
    static const QString KEY_TRANSFER_FILE_COUNT = "transferFileCount";
    static const QString KEY_TRANSFER_TOTAL_FILES = "transferTotalFiles";
    static const QString KEY_FILE_SUFFIX = "file_suffix";
    static const QString KEY_FILE_EXECUTABLE = "file_executable";
    static const QString KEY_IS_DIR = "is_dir";
    static const QString KEY_FILE_LAST_MOD_TIME = "file_last_mod_time";
    static const QString KEY_RECEIVER_PWD = "receiver_pwd";
    static const QString KEY_MSGTYPE = "msgType";
    static const QString KEY_PATH = "path";
    static const QString KEY_PATH_CLI = "path_cli";
    static const QString KEY_PATH_CTL = "path_ctl";
    static const QString KEY_ACTION = "action";
    static const QString KEY_ERROR = "error";
    static const QString KEY_ROWS = "rows";
    static const QString KEY_COLS = "cols";
    static const QString KEY_ENCODING = "encoding";
    static const QString KEY_OS = "os";
    static const QString KEY_SHELL = "shell";
    static const QString KEY_TERMINAL_MODE = "terminalMode";
    static const QString KEY_PATH_TRACKING = "pathTracking";

    static const QString ROLE_CLI = "cli";
    static const QString ROLE_CTL = "ctl";
    static const QString ROLE_SERVER = "server";

    static const QString TYPE_LIST_FOLDER_FILES = "listFolderFiles";
    static const QString TYPE_UPLOAD_FILE = "upload_file";
    static const QString TYPE_UPLOAD_FILE_RES = "upload_file_res";
    static const QString TYPE_DOWNLOAD_FILE = "download_file";
    static const QString TYPE_DOWNLOAD_FILE_RES = "download_file_res";
    static const QString TYPE_FILE_LIST = "file_list";
    static const QString TYPE_FILE_DOWNLOAD = "file_download";
    static const QString TYPE_FILE_UPLOAD = "file_upload";
    static const QString TYPE_FILE_TRANSFER_PROGRESS = "file_transfer_progress";
    static const QString TYPE_FILE_TRANSFER_CANCEL = "file_transfer_cancel";

    static const QString TYPE_OFFER = "offer";
    static const QString TYPE_ANSWER = "answer";
    static const QString TYPE_CANDIDATE = "candidate";
    static const QString TYPE_FILE = "file_airan";
    static const QString TYPE_FILE_TEXT = "file_text_airan";
    static const QString TYPE_VIDEO = "video_airan";
    static const QString TYPE_VIDEO_MSID = "video_stream1_airan";
    static const QString TYPE_AUDIO = "audio_airan";
    static const QString TYPE_INPUT = "input_airan";
    static const QString TYPE_DIR = "dir";
    static const QString TYPE_CONNECT = "connect";
    static const QString TYPE_CONNECTED = "connected";
    static const QString TYPE_DEVICE_ID_CONFLICT = "deviceIdConflict";
    static const QString TYPE_KEYFRAME_REQUEST = "keyframe_request";
    static const QString TYPE_KEYFRAME_RESPONSE = "keyframe_response";
    static const QString TYPE_ERROR = "error";
    static const QString ERROR_PASSWORD_INCORRECT = "password_incorrect";

    static const QString TYPE_KEYBOARD = "keyboard";
    static const QString TYPE_MOUSE = "mouse";
    static const QString TYPE_STREAM_CONFIG = "stream_config";
    static const QString TYPE_VIDEO_ADAPT_FEEDBACK = "video_adapt_feedback";
    static const QString TYPE_AUDIO_CAPTURE = "audio_capture";
    static const QString TYPE_SWITCH_SCREEN = "switch_screen";
    static const QString TYPE_CONTROL_HEARTBEAT = "control_heartbeat";
    static const QString TYPE_REMOTE_OPERATION = "remote_operation";
    static const QString TYPE_ANDROID_NAVIGATION = "android_navigation";
    static const QString TYPE_DESKTOP_STATE = "desktop_state";
    static const QString TYPE_RUN_FILE = "run_file";
    static const QString TYPE_TERMINAL_START = "terminal_start";
    static const QString TYPE_TERMINAL_INPUT = "terminal_input";
    static const QString TYPE_TERMINAL_RESIZE = "terminal_resize";
    static const QString TYPE_TERMINAL_OUTPUT = "terminal_output";
    static const QString TYPE_TERMINAL_STOP = "terminal_stop";
    static const QString TYPE_TERMINAL_CLOSED = "terminal_closed";
    static const QString TYPE_TERMINAL_ERROR = "terminal_error";
    static const QString TYPE_TERMINAL_INFO = "terminal_info";

    static const QString KEY_KEY = "key";
    static const QString KEY_TEXT = "text";
    static const QString KEY_DWFLAGS = "dwFlags";
    static const QString KEY_X = "x";
    static const QString KEY_Y = "y";
    static const QString KEY_DOWN = "down";
    static const QString KEY_UP = "up";
    static const QString KEY_MOVE = "move";
    static const QString KEY_WHEEL = "wheel";
    static const QString KEY_DOUBLECLICK = "doubleClick";
    static const QString KEY_BUTTON = "button";
    static const QString KEY_MOUSEDATA = "mouseData";
    static const QString KEY_CAPTURE_BACKEND = "captureBackend";
    static const QString KEY_CAPTURE_BACKENDS = "captureBackends";
    static const QString KEY_QUALITY = "quality";
    static const QString KEY_BITRATE = "bitrate";
    static const QString KEY_BITRATE_PROFILE = "bitrateProfile";
    static const QString KEY_NETWORK_PATH = "networkPath";
    static const QString KEY_LOCKED = "locked";
    static const QString KEY_ENABLED = "enabled";
    static const QString KEY_AUDIO_MODE = "audioMode";
    static const QString KEY_MESSAGE = "message";
    static const QString KEY_FOLDER_MOUNTED = "mounted";
    static const QString KEY_FOLDER_FILES = "folderFiles";
    static const QString START_CAPTURE = "startCapture";
    static const QString STOP_CAPTURE = "stopCapture";
    static const QString START_MEDIA_STREAM = "startMediaStream";
    static const QString STOP_MEDIA_STREAM = "stopMediaStream";

    static const QString FOLDER_HOME = "home";

    static const QString KEY_IMPLEMENTATION = "implementation";
    static const QString KEY_IMPL_TYPE = "implType";
    static const QString KEY_IS_HARDWARE = "isHardware";
    static const QString KEY_OPENABLE = "openable";
    static const QString KEY_PRIORITY = "priority";
    static const QString KEY_FRAMES_TESTED = "framesTested";
    static const QString KEY_STABLE_FRAMES = "stableFrames";
    static const QString KEY_ENCODE = "encode";
    static const QString KEY_DECODE = "decode";
    static const QString KEY_ZERO_COPY = "zeroCopy";
    static const QString KEY_CODEC = "codec";
    static const QString KEY_PROFILE = "profile";
    static const QString KEY_PACKETIZATION_MODE = "packetizationMode";
    static const QString KEY_PROFILE_LEVEL_ID = "profileLevelId";
    static const QString KEY_ALLOW_HIGH_PERFORMANCE = "allowHighPerformance";
    static const QString KEY_CODECS = "codecs";
    static const QString KEY_SUPPORTED_CODECS = "supportedCodecs";
    static const QString KEY_SUPPORTED_PROFILES = "supportedProfiles";
    static const QString KEY_IMPLEMENTATIONS = "implementations";
    static const QString KEY_PATHS = "paths";
    static const QString KEY_MAX_STABLE = "maxStable";
    static const QString KEY_RECOMMENDED = "recommended";
    static const QString KEY_RECOMMENDED_SEND = "recommendedSend";
    static const QString KEY_RECOMMENDED_RECEIVE = "recommendedReceive";
    static const QString KEY_SEND_HINT = "sendHint";
    static const QString KEY_RECEIVE_HINT = "receiveHint";
    static const QString KEY_CODEC_MODE = "codecMode";
    static const QString KEY_SELECTED_CODEC = "selectedCodec";
    static const QString KEY_AVAILABLE = "available";
    static const QString KEY_STABLE = "stable";
    static const QString KEY_EXPERIMENTAL = "experimental";
    static const QString KEY_DISABLED = "disabled";
    static const QString KEY_PREFERRED = "preferred";
}

#endif /* PROTOCOL_CONSTANTS_H */
