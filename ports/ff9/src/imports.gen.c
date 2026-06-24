// imports_unity.gen.c — GERADO. passthrough via dlsym; resto = stub log.
#include "so_util.h"
#include <stdio.h>
#include <stdint.h>
#include <dlfcn.h>

// stubs (logam nome, 1as 2 vezes, retornam 0)
long stub_AAsset_close(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AAsset_close\\n"); return 0; }
long stub_AAsset_getLength(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AAsset_getLength\\n"); return 0; }
long stub_AAssetManager_fromJava(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AAssetManager_fromJava\\n"); return 0; }
long stub_AAssetManager_open(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AAssetManager_open\\n"); return 0; }
long stub_AAsset_read(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AAsset_read\\n"); return 0; }
long stub_AAsset_seek(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AAsset_seek\\n"); return 0; }
long stub_accept(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] accept\\n"); return 0; }
long stub_AHardwareBuffer_acquire(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AHardwareBuffer_acquire\\n"); return 0; }
long stub_AHardwareBuffer_describe(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AHardwareBuffer_describe\\n"); return 0; }
long stub_AHardwareBuffer_release(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AHardwareBuffer_release\\n"); return 0; }
long stub_AImage_delete(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AImage_delete\\n"); return 0; }
long stub_AImage_deleteAsync(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AImage_deleteAsync\\n"); return 0; }
long stub_AImage_getHardwareBuffer(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AImage_getHardwareBuffer\\n"); return 0; }
long stub_AImage_getTimestamp(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AImage_getTimestamp\\n"); return 0; }
long stub_AImage_getWidth(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AImage_getWidth\\n"); return 0; }
long stub_AImageReader_acquireLatestImage(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AImageReader_acquireLatestImage\\n"); return 0; }
long stub_AImageReader_delete(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AImageReader_delete\\n"); return 0; }
long stub_AImageReader_getWindow(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AImageReader_getWindow\\n"); return 0; }
long stub_AImageReader_newWithUsage(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AImageReader_newWithUsage\\n"); return 0; }
long stub_AImageReader_setBufferRemovedListener(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AImageReader_setBufferRemovedListener\\n"); return 0; }
long stub_AImageReader_setImageListener(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AImageReader_setImageListener\\n"); return 0; }
long stub_ALooper_acquire(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ALooper_acquire\\n"); return 0; }
long stub_ALooper_forThread(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ALooper_forThread\\n"); return 0; }
long stub_ALooper_pollOnce(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ALooper_pollOnce\\n"); return 0; }
long stub_ALooper_prepare(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ALooper_prepare\\n"); return 0; }
long stub_ALooper_release(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ALooper_release\\n"); return 0; }
long stub_ALooper_wake(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ALooper_wake\\n"); return 0; }
long stub_AMediaCodec_configure(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AMediaCodec_configure\\n"); return 0; }
long stub_AMediaCodec_createDecoderByType(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AMediaCodec_createDecoderByType\\n"); return 0; }
long stub_AMediaCodec_delete(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AMediaCodec_delete\\n"); return 0; }
long stub_AMediaCodec_dequeueInputBuffer(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AMediaCodec_dequeueInputBuffer\\n"); return 0; }
long stub_AMediaCodec_dequeueOutputBuffer(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AMediaCodec_dequeueOutputBuffer\\n"); return 0; }
long stub_AMediaCodec_flush(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AMediaCodec_flush\\n"); return 0; }
long stub_AMediaCodec_getInputBuffer(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AMediaCodec_getInputBuffer\\n"); return 0; }
long stub_AMediaCodec_getOutputBuffer(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AMediaCodec_getOutputBuffer\\n"); return 0; }
long stub_AMediaCodec_getOutputFormat(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AMediaCodec_getOutputFormat\\n"); return 0; }
long stub_AMediaCodec_queueInputBuffer(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AMediaCodec_queueInputBuffer\\n"); return 0; }
long stub_AMediaCodec_releaseOutputBuffer(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AMediaCodec_releaseOutputBuffer\\n"); return 0; }
long stub_AMediaCodec_setOutputSurface(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AMediaCodec_setOutputSurface\\n"); return 0; }
long stub_AMediaCodec_start(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AMediaCodec_start\\n"); return 0; }
long stub_AMediaCodec_stop(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AMediaCodec_stop\\n"); return 0; }
long stub_AMediaDataSource_delete(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AMediaDataSource_delete\\n"); return 0; }
long stub_AMediaDataSource_new(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AMediaDataSource_new\\n"); return 0; }
long stub_AMediaDataSource_setClose(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AMediaDataSource_setClose\\n"); return 0; }
long stub_AMediaDataSource_setGetSize(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AMediaDataSource_setGetSize\\n"); return 0; }
long stub_AMediaDataSource_setReadAt(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AMediaDataSource_setReadAt\\n"); return 0; }
long stub_AMediaDataSource_setUserdata(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AMediaDataSource_setUserdata\\n"); return 0; }
long stub_AMediaExtractor_advance(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AMediaExtractor_advance\\n"); return 0; }
long stub_AMediaExtractor_delete(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AMediaExtractor_delete\\n"); return 0; }
long stub_AMediaExtractor_getSampleTime(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AMediaExtractor_getSampleTime\\n"); return 0; }
long stub_AMediaExtractor_getSampleTrackIndex(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AMediaExtractor_getSampleTrackIndex\\n"); return 0; }
long stub_AMediaExtractor_getTrackCount(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AMediaExtractor_getTrackCount\\n"); return 0; }
long stub_AMediaExtractor_getTrackFormat(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AMediaExtractor_getTrackFormat\\n"); return 0; }
long stub_AMediaExtractor_new(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AMediaExtractor_new\\n"); return 0; }
long stub_AMediaExtractor_readSampleData(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AMediaExtractor_readSampleData\\n"); return 0; }
long stub_AMediaExtractor_seekTo(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AMediaExtractor_seekTo\\n"); return 0; }
long stub_AMediaExtractor_selectTrack(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AMediaExtractor_selectTrack\\n"); return 0; }
long stub_AMediaExtractor_setDataSource(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AMediaExtractor_setDataSource\\n"); return 0; }
long stub_AMediaExtractor_setDataSourceCustom(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AMediaExtractor_setDataSourceCustom\\n"); return 0; }
long stub_AMediaExtractor_setDataSourceFd(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AMediaExtractor_setDataSourceFd\\n"); return 0; }
long stub_AMediaFormat_delete(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AMediaFormat_delete\\n"); return 0; }
long stub_AMediaFormat_getFloat(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AMediaFormat_getFloat\\n"); return 0; }
long stub_AMediaFormat_getInt32(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AMediaFormat_getInt32\\n"); return 0; }
long stub_AMediaFormat_getInt64(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AMediaFormat_getInt64\\n"); return 0; }
long stub_AMediaFormat_getString(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AMediaFormat_getString\\n"); return 0; }
long stub_AMEDIAFORMAT_KEY_CHANNEL_COUNT(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AMEDIAFORMAT_KEY_CHANNEL_COUNT\\n"); return 0; }
long stub_AMEDIAFORMAT_KEY_COLOR_FORMAT(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AMEDIAFORMAT_KEY_COLOR_FORMAT\\n"); return 0; }
long stub_AMEDIAFORMAT_KEY_COLOR_RANGE(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AMEDIAFORMAT_KEY_COLOR_RANGE\\n"); return 0; }
long stub_AMEDIAFORMAT_KEY_COLOR_STANDARD(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AMEDIAFORMAT_KEY_COLOR_STANDARD\\n"); return 0; }
long stub_AMEDIAFORMAT_KEY_DURATION(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AMEDIAFORMAT_KEY_DURATION\\n"); return 0; }
long stub_AMEDIAFORMAT_KEY_ENCODER_DELAY(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AMEDIAFORMAT_KEY_ENCODER_DELAY\\n"); return 0; }
long stub_AMEDIAFORMAT_KEY_FRAME_RATE(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AMEDIAFORMAT_KEY_FRAME_RATE\\n"); return 0; }
long stub_AMEDIAFORMAT_KEY_HEIGHT(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AMEDIAFORMAT_KEY_HEIGHT\\n"); return 0; }
long stub_AMEDIAFORMAT_KEY_LANGUAGE(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AMEDIAFORMAT_KEY_LANGUAGE\\n"); return 0; }
long stub_AMEDIAFORMAT_KEY_MIME(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AMEDIAFORMAT_KEY_MIME\\n"); return 0; }
long stub_AMEDIAFORMAT_KEY_ROTATION(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AMEDIAFORMAT_KEY_ROTATION\\n"); return 0; }
long stub_AMEDIAFORMAT_KEY_SAMPLE_RATE(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AMEDIAFORMAT_KEY_SAMPLE_RATE\\n"); return 0; }
long stub_AMEDIAFORMAT_KEY_SLICE_HEIGHT(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AMEDIAFORMAT_KEY_SLICE_HEIGHT\\n"); return 0; }
long stub_AMEDIAFORMAT_KEY_STRIDE(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AMEDIAFORMAT_KEY_STRIDE\\n"); return 0; }
long stub_AMEDIAFORMAT_KEY_WIDTH(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AMEDIAFORMAT_KEY_WIDTH\\n"); return 0; }
long stub_AMediaFormat_setInt32(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] AMediaFormat_setInt32\\n"); return 0; }
long stub_ANativeWindow_acquire(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ANativeWindow_acquire\\n"); return 0; }
long stub_ANativeWindow_fromSurface(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ANativeWindow_fromSurface\\n"); return 0; }
long stub_ANativeWindow_getHeight(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ANativeWindow_getHeight\\n"); return 0; }
long stub_ANativeWindow_getWidth(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ANativeWindow_getWidth\\n"); return 0; }
long stub_ANativeWindow_release(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ANativeWindow_release\\n"); return 0; }
long stub_ANativeWindow_setBuffersGeometry(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ANativeWindow_setBuffersGeometry\\n"); return 0; }
long stub_ANativeWindow_toSurface(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ANativeWindow_toSurface\\n"); return 0; }
long stub___android_log_print(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __android_log_print\\n"); return 0; }
long stub___android_log_vprint(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __android_log_vprint\\n"); return 0; }
long stub___android_log_write(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __android_log_write\\n"); return 0; }
long stub_android_set_abort_message(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] android_set_abort_message\\n"); return 0; }
long stub_ASensorEventQueue_disableSensor(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ASensorEventQueue_disableSensor\\n"); return 0; }
long stub_ASensorEventQueue_enableSensor(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ASensorEventQueue_enableSensor\\n"); return 0; }
long stub_ASensorEventQueue_getEvents(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ASensorEventQueue_getEvents\\n"); return 0; }
long stub_ASensorEventQueue_hasEvents(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ASensorEventQueue_hasEvents\\n"); return 0; }
long stub_ASensorEventQueue_setEventRate(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ASensorEventQueue_setEventRate\\n"); return 0; }
long stub_ASensor_getMinDelay(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ASensor_getMinDelay\\n"); return 0; }
long stub_ASensor_getName(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ASensor_getName\\n"); return 0; }
long stub_ASensor_getResolution(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ASensor_getResolution\\n"); return 0; }
long stub_ASensor_getType(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ASensor_getType\\n"); return 0; }
long stub_ASensor_getVendor(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ASensor_getVendor\\n"); return 0; }
long stub_ASensorManager_createEventQueue(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ASensorManager_createEventQueue\\n"); return 0; }
long stub_ASensorManager_destroyEventQueue(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ASensorManager_destroyEventQueue\\n"); return 0; }
long stub_ASensorManager_getDefaultSensor(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ASensorManager_getDefaultSensor\\n"); return 0; }
long stub_ASensorManager_getInstance(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ASensorManager_getInstance\\n"); return 0; }
long stub_ASensorManager_getSensorList(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ASensorManager_getSensorList\\n"); return 0; }
long stub_basename(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] basename\\n"); return 0; }
long stub_bind(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] bind\\n"); return 0; }
long stub_chmod(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] chmod\\n"); return 0; }
long stub_closelog(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] closelog\\n"); return 0; }
long stub_connect(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] connect\\n"); return 0; }
long stub__ctype_(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] _ctype_\\n"); return 0; }
long stub___ctype_get_mb_cur_max(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __ctype_get_mb_cur_max\\n"); return 0; }
long stub___cxa_atexit(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __cxa_atexit\\n"); return 0; }
long stub___cxa_finalize(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __cxa_finalize\\n"); return 0; }
long stub___cxa_pure_virtual(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __cxa_pure_virtual\\n"); return 0; }
long stub_dladdr(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] dladdr\\n"); return 0; }
long stub_dl_iterate_phdr(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] dl_iterate_phdr\\n"); return 0; }
long stub___errno(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __errno\\n"); return 0; }
long stub_exp2f(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] exp2f\\n"); return 0; }
long stub_fchmod(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] fchmod\\n"); return 0; }
long stub___FD_ISSET_chk(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __FD_ISSET_chk\\n"); return 0; }
long stub___FD_SET_chk(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __FD_SET_chk\\n"); return 0; }
long stub_flock(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] flock\\n"); return 0; }
long stub_fnmatch(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] fnmatch\\n"); return 0; }
long stub_freeaddrinfo(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] freeaddrinfo\\n"); return 0; }
long stub_fscanf(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] fscanf\\n"); return 0; }
long stub_futimens(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] futimens\\n"); return 0; }
long stub_getaddrinfo(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] getaddrinfo\\n"); return 0; }
long stub_getegid(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] getegid\\n"); return 0; }
long stub_geteuid(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] geteuid\\n"); return 0; }
long stub_gethostbyaddr(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] gethostbyaddr\\n"); return 0; }
long stub_gethostbyname(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] gethostbyname\\n"); return 0; }
long stub_gethostname(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] gethostname\\n"); return 0; }
long stub_getnameinfo(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] getnameinfo\\n"); return 0; }
long stub_getpeername(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] getpeername\\n"); return 0; }
long stub_getpriority(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] getpriority\\n"); return 0; }
long stub_getpwuid(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] getpwuid\\n"); return 0; }
long stub_getpwuid_r(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] getpwuid_r\\n"); return 0; }
long stub_getsockname(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] getsockname\\n"); return 0; }
long stub_getsockopt(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] getsockopt\\n"); return 0; }
long stub_if_nametoindex(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] if_nametoindex\\n"); return 0; }
long stub_inet_addr(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] inet_addr\\n"); return 0; }
long stub_inet_ntop(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] inet_ntop\\n"); return 0; }
long stub_inet_pton(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] inet_pton\\n"); return 0; }
long stub_inflate(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] inflate\\n"); return 0; }
long stub_inflateEnd(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] inflateEnd\\n"); return 0; }
long stub_inflateInit2_(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] inflateInit2_\\n"); return 0; }
long stub_isatty(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] isatty\\n"); return 0; }
long stub_isdigit_l(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] isdigit_l\\n"); return 0; }
long stub_islower_l(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] islower_l\\n"); return 0; }
long stub_isupper_l(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] isupper_l\\n"); return 0; }
long stub_iswalpha_l(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] iswalpha_l\\n"); return 0; }
long stub_iswblank_l(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] iswblank_l\\n"); return 0; }
long stub_iswcntrl_l(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] iswcntrl_l\\n"); return 0; }
long stub_iswdigit_l(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] iswdigit_l\\n"); return 0; }
long stub_iswlower_l(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] iswlower_l\\n"); return 0; }
long stub_iswprint_l(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] iswprint_l\\n"); return 0; }
long stub_iswpunct_l(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] iswpunct_l\\n"); return 0; }
long stub_iswspace_l(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] iswspace_l\\n"); return 0; }
long stub_iswupper_l(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] iswupper_l\\n"); return 0; }
long stub_iswxdigit_l(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] iswxdigit_l\\n"); return 0; }
long stub_isxdigit_l(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] isxdigit_l\\n"); return 0; }
long stub_ldexpf(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ldexpf\\n"); return 0; }
long stub_listen(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] listen\\n"); return 0; }
long stub_lldiv(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] lldiv\\n"); return 0; }
long stub_logb(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] logb\\n"); return 0; }
long stub_lrand48(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] lrand48\\n"); return 0; }
long stub_mbsrtowcs(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] mbsrtowcs\\n"); return 0; }
long stub_mbtowc(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] mbtowc\\n"); return 0; }
long stub_memalign(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] memalign\\n"); return 0; }
long stub___memcpy_chk(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __memcpy_chk\\n"); return 0; }
long stub___memmove_chk(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __memmove_chk\\n"); return 0; }
long stub_modff(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] modff\\n"); return 0; }
long stub_openlog(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] openlog\\n"); return 0; }
long stub_ptrace(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ptrace\\n"); return 0; }
long stub_raise(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] raise\\n"); return 0; }
long stub_recv(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] recv\\n"); return 0; }
long stub_recvfrom(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] recvfrom\\n"); return 0; }
long stub_recvmsg(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] recvmsg\\n"); return 0; }
long stub_scalbn(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] scalbn\\n"); return 0; }
long stub_sched_getaffinity(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] sched_getaffinity\\n"); return 0; }
long stub_sched_setaffinity(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] sched_setaffinity\\n"); return 0; }
long stub_send(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] send\\n"); return 0; }
long stub_sendfile(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] sendfile\\n"); return 0; }
long stub_sendmsg(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] sendmsg\\n"); return 0; }
long stub_setpriority(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] setpriority\\n"); return 0; }
long stub_setsockopt(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] setsockopt\\n"); return 0; }
long stub___sF(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __sF\\n"); return 0; }
long stub_shutdown(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] shutdown\\n"); return 0; }
long stub_sigaction(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] sigaction\\n"); return 0; }
long stub_signal(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] signal\\n"); return 0; }
long stub_sigsuspend(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] sigsuspend\\n"); return 0; }
long stub_sincos(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] sincos\\n"); return 0; }
long stub_sincosf(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] sincosf\\n"); return 0; }
long stub_slCreateEngine(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] slCreateEngine\\n"); return 0; }
long stub_SL_IID_3DCOMMIT(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] SL_IID_3DCOMMIT\\n"); return 0; }
long stub_SL_IID_3DDOPPLER(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] SL_IID_3DDOPPLER\\n"); return 0; }
long stub_SL_IID_3DGROUPING(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] SL_IID_3DGROUPING\\n"); return 0; }
long stub_SL_IID_3DLOCATION(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] SL_IID_3DLOCATION\\n"); return 0; }
long stub_SL_IID_3DMACROSCOPIC(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] SL_IID_3DMACROSCOPIC\\n"); return 0; }
long stub_SL_IID_3DSOURCE(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] SL_IID_3DSOURCE\\n"); return 0; }
long stub_SL_IID_ANDROIDCONFIGURATION(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] SL_IID_ANDROIDCONFIGURATION\\n"); return 0; }
long stub_SL_IID_ANDROIDEFFECT(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] SL_IID_ANDROIDEFFECT\\n"); return 0; }
long stub_SL_IID_ANDROIDEFFECTCAPABILITIES(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] SL_IID_ANDROIDEFFECTCAPABILITIES\\n"); return 0; }
long stub_SL_IID_ANDROIDEFFECTSEND(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] SL_IID_ANDROIDEFFECTSEND\\n"); return 0; }
long stub_SL_IID_ANDROIDSIMPLEBUFFERQUEUE(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] SL_IID_ANDROIDSIMPLEBUFFERQUEUE\\n"); return 0; }
long stub_SL_IID_AUDIODECODERCAPABILITIES(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] SL_IID_AUDIODECODERCAPABILITIES\\n"); return 0; }
long stub_SL_IID_AUDIOENCODER(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] SL_IID_AUDIOENCODER\\n"); return 0; }
long stub_SL_IID_AUDIOENCODERCAPABILITIES(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] SL_IID_AUDIOENCODERCAPABILITIES\\n"); return 0; }
long stub_SL_IID_AUDIOIODEVICECAPABILITIES(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] SL_IID_AUDIOIODEVICECAPABILITIES\\n"); return 0; }
long stub_SL_IID_BASSBOOST(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] SL_IID_BASSBOOST\\n"); return 0; }
long stub_SL_IID_BUFFERQUEUE(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] SL_IID_BUFFERQUEUE\\n"); return 0; }
long stub_SL_IID_DEVICEVOLUME(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] SL_IID_DEVICEVOLUME\\n"); return 0; }
long stub_SL_IID_DYNAMICINTERFACEMANAGEMENT(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] SL_IID_DYNAMICINTERFACEMANAGEMENT\\n"); return 0; }
long stub_SL_IID_DYNAMICSOURCE(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] SL_IID_DYNAMICSOURCE\\n"); return 0; }
long stub_SL_IID_EFFECTSEND(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] SL_IID_EFFECTSEND\\n"); return 0; }
long stub_SL_IID_ENGINE(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] SL_IID_ENGINE\\n"); return 0; }
long stub_SL_IID_ENGINECAPABILITIES(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] SL_IID_ENGINECAPABILITIES\\n"); return 0; }
long stub_SL_IID_ENVIRONMENTALREVERB(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] SL_IID_ENVIRONMENTALREVERB\\n"); return 0; }
long stub_SL_IID_EQUALIZER(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] SL_IID_EQUALIZER\\n"); return 0; }
long stub_SL_IID_LED(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] SL_IID_LED\\n"); return 0; }
long stub_SL_IID_METADATAEXTRACTION(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] SL_IID_METADATAEXTRACTION\\n"); return 0; }
long stub_SL_IID_METADATATRAVERSAL(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] SL_IID_METADATATRAVERSAL\\n"); return 0; }
long stub_SL_IID_MIDIMESSAGE(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] SL_IID_MIDIMESSAGE\\n"); return 0; }
long stub_SL_IID_MIDIMUTESOLO(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] SL_IID_MIDIMUTESOLO\\n"); return 0; }
long stub_SL_IID_MIDITEMPO(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] SL_IID_MIDITEMPO\\n"); return 0; }
long stub_SL_IID_MIDITIME(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] SL_IID_MIDITIME\\n"); return 0; }
long stub_SL_IID_MUTESOLO(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] SL_IID_MUTESOLO\\n"); return 0; }
long stub_SL_IID_NULL(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] SL_IID_NULL\\n"); return 0; }
long stub_SL_IID_OBJECT(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] SL_IID_OBJECT\\n"); return 0; }
long stub_SL_IID_OUTPUTMIX(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] SL_IID_OUTPUTMIX\\n"); return 0; }
long stub_SL_IID_PITCH(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] SL_IID_PITCH\\n"); return 0; }
long stub_SL_IID_PLAY(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] SL_IID_PLAY\\n"); return 0; }
long stub_SL_IID_PLAYBACKRATE(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] SL_IID_PLAYBACKRATE\\n"); return 0; }
long stub_SL_IID_PREFETCHSTATUS(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] SL_IID_PREFETCHSTATUS\\n"); return 0; }
long stub_SL_IID_PRESETREVERB(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] SL_IID_PRESETREVERB\\n"); return 0; }
long stub_SL_IID_RATEPITCH(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] SL_IID_RATEPITCH\\n"); return 0; }
long stub_SL_IID_RECORD(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] SL_IID_RECORD\\n"); return 0; }
long stub_SL_IID_SEEK(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] SL_IID_SEEK\\n"); return 0; }
long stub_SL_IID_THREADSYNC(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] SL_IID_THREADSYNC\\n"); return 0; }
long stub_SL_IID_VIBRA(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] SL_IID_VIBRA\\n"); return 0; }
long stub_SL_IID_VIRTUALIZER(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] SL_IID_VIRTUALIZER\\n"); return 0; }
long stub_SL_IID_VISUALIZATION(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] SL_IID_VISUALIZATION\\n"); return 0; }
long stub_SL_IID_VOLUME(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] SL_IID_VOLUME\\n"); return 0; }
long stub_socket(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] socket\\n"); return 0; }
long stub_srand48(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] srand48\\n"); return 0; }
long stub___stack_chk_fail(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __stack_chk_fail\\n"); return 0; }
long stub___stack_chk_guard(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __stack_chk_guard\\n"); return 0; }
long stub_stpcpy(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] stpcpy\\n"); return 0; }
long stub_strcasestr(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] strcasestr\\n"); return 0; }
long stub_strcoll_l(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] strcoll_l\\n"); return 0; }
long stub_strerror_r(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] strerror_r\\n"); return 0; }
long stub_strftime_l(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] strftime_l\\n"); return 0; }
long stub_strlcpy(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] strlcpy\\n"); return 0; }
long stub___strlen_chk(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __strlen_chk\\n"); return 0; }
long stub_strnlen(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] strnlen\\n"); return 0; }
long stub_strtok_r(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] strtok_r\\n"); return 0; }
long stub_strtold(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] strtold\\n"); return 0; }
long stub_strtold_l(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] strtold_l\\n"); return 0; }
long stub_strtoll_l(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] strtoll_l\\n"); return 0; }
long stub_strtoull_l(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] strtoull_l\\n"); return 0; }
long stub_strxfrm_l(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] strxfrm_l\\n"); return 0; }
long stub_syscall(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] syscall\\n"); return 0; }
long stub_syslog(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] syslog\\n"); return 0; }
long stub___system_property_find(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __system_property_find\\n"); return 0; }
long stub___system_property_get(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __system_property_get\\n"); return 0; }
long stub___system_property_read(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __system_property_read\\n"); return 0; }
long stub_tolower_l(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] tolower_l\\n"); return 0; }
long stub_toupper_l(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] toupper_l\\n"); return 0; }
long stub_towlower_l(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] towlower_l\\n"); return 0; }
long stub_towupper_l(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] towupper_l\\n"); return 0; }
long stub_uname(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] uname\\n"); return 0; }
long stub_utime(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] utime\\n"); return 0; }
long stub_utimes(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] utimes\\n"); return 0; }
long stub_vasprintf(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] vasprintf\\n"); return 0; }
long stub___vsnprintf_chk(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __vsnprintf_chk\\n"); return 0; }
long stub___vsprintf_chk(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __vsprintf_chk\\n"); return 0; }
long stub_vsscanf(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] vsscanf\\n"); return 0; }
long stub_wcscoll_l(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] wcscoll_l\\n"); return 0; }
long stub_wcstold(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] wcstold\\n"); return 0; }
long stub_wcsxfrm_l(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] wcsxfrm_l\\n"); return 0; }
long stub__ZdaPv(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] _ZdaPv\\n"); return 0; }
long stub__Znam(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] _Znam\\n"); return 0; }
long stub__ZTH15gDeferredAction(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] _ZTH15gDeferredAction\\n"); return 0; }

// flag: 1 = passthrough (resolver via dlsym), 0 = stub ja setado
static const char *passthrough_names[] = {
  "abort",
  "access",
  "acos",
  "acosf",
  "asinf",
  "atan",
  "atan2",
  "atan2f",
  "atanf",
  "atof",
  "atoi",
  "atol",
  "bsearch",
  "btowc",
  "calloc",
  "clearerr",
  "clock",
  "clock_getres",
  "clock_gettime",
  "close",
  "closedir",
  "cos",
  "cosf",
  "difftime",
  "div",
  "dlclose",
  "dlerror",
  "dlopen",
  "dlsym",
  "dup",
  "dup2",
  "eglChooseConfig",
  "eglCreateContext",
  "eglCreatePbufferSurface",
  "eglCreateWindowSurface",
  "eglDestroyContext",
  "eglDestroySurface",
  "eglGetConfigAttrib",
  "eglGetCurrentContext",
  "eglGetCurrentSurface",
  "eglGetDisplay",
  "eglGetError",
  "eglGetProcAddress",
  "eglInitialize",
  "eglMakeCurrent",
  "eglQueryString",
  "eglQuerySurface",
  "eglSurfaceAttrib",
  "eglSwapBuffers",
  "eglSwapInterval",
  "eglTerminate",
  "exit",
  "exp",
  "expf",
  "fclose",
  "fcntl",
  "fdopen",
  "feof",
  "ferror",
  "fflush",
  "fgets",
  "fileno",
  "fmod",
  "fmodf",
  "fopen",
  "fprintf",
  "fputc",
  "fputs",
  "fread",
  "free",
  "freelocale",
  "fseek",
  "fseeko",
  "fstat",
  "ftell",
  "ftello",
  "ftruncate",
  "fwrite",
  "getauxval",
  "getcwd",
  "getenv",
  "getpagesize",
  "getpid",
  "gettid",
  "gettimeofday",
  "getuid",
  "gmtime",
  "gmtime_r",
  "hypot",
  "ioctl",
  "ldexp",
  "link",
  "localeconv",
  "localtime",
  "localtime_r",
  "log",
  "log10",
  "log10f",
  "log2",
  "logf",
  "longjmp",
  "lseek",
  "lseek64",
  "lstat",
  "madvise",
  "malloc",
  "mbrlen",
  "mbrtowc",
  "mbsnrtowcs",
  "memchr",
  "memcmp",
  "memcpy",
  "memmove",
  "memrchr",
  "memset",
  "mkdir",
  "mktime",
  "mmap",
  "modf",
  "mprotect",
  "munmap",
  "nanosleep",
  "newlocale",
  "open",
  "opendir",
  "pipe",
  "poll",
  "posix_memalign",
  "pow",
  "powf",
  "prctl",
  "printf",
  "pthread_atfork",
  "pthread_attr_destroy",
  "pthread_attr_getstack",
  "pthread_attr_init",
  "pthread_attr_setdetachstate",
  "pthread_attr_setstacksize",
  "pthread_condattr_destroy",
  "pthread_condattr_init",
  "pthread_condattr_setclock",
  "pthread_cond_broadcast",
  "pthread_cond_destroy",
  "pthread_cond_init",
  "pthread_cond_signal",
  "pthread_cond_timedwait",
  "pthread_cond_wait",
  "pthread_create",
  "pthread_detach",
  "pthread_equal",
  "pthread_exit",
  "pthread_getattr_np",
  "pthread_getspecific",
  "pthread_join",
  "pthread_key_create",
  "pthread_key_delete",
  "pthread_kill",
  "pthread_mutexattr_destroy",
  "pthread_mutexattr_init",
  "pthread_mutexattr_settype",
  "pthread_mutex_destroy",
  "pthread_mutex_init",
  "pthread_mutex_lock",
  "pthread_mutex_trylock",
  "pthread_mutex_unlock",
  "pthread_once",
  "pthread_rwlock_init",
  "pthread_rwlock_rdlock",
  "pthread_rwlock_unlock",
  "pthread_rwlock_wrlock",
  "pthread_self",
  "pthread_setname_np",
  "pthread_setspecific",
  "pthread_sigmask",
  "puts",
  "qsort",
  "rand",
  "read",
  "readdir",
  "readlink",
  "realloc",
  "realpath",
  "remove",
  "rename",
  "rewind",
  "rmdir",
  "sched_yield",
  "select",
  "sem_destroy",
  "sem_getvalue",
  "sem_init",
  "sem_post",
  "sem_timedwait",
  "sem_wait",
  "setbuf",
  "setenv",
  "setjmp",
  "setlocale",
  "setvbuf",
  "sigaddset",
  "sigaltstack",
  "sigdelset",
  "sigemptyset",
  "sigfillset",
  "sin",
  "sinf",
  "snprintf",
  "sprintf",
  "sqrtf",
  "srand",
  "sscanf",
  "stat",
  "statfs",
  "strcasecmp",
  "strcat",
  "strchr",
  "strcmp",
  "strcpy",
  "strcspn",
  "strdup",
  "strerror",
  "strftime",
  "strlen",
  "strncasecmp",
  "strncmp",
  "strncpy",
  "strrchr",
  "strspn",
  "strstr",
  "strtod",
  "strtof",
  "strtol",
  "strtoll",
  "strtoul",
  "strtoull",
  "swprintf",
  "symlink",
  "sysconf",
  "tan",
  "tanf",
  "time",
  "toupper",
  "towlower",
  "truncate",
  "unlink",
  "unsetenv",
  "uselocale",
  "usleep",
  "vfprintf",
  "vprintf",
  "vsnprintf",
  "wcrtomb",
  "wcslen",
  "wcsnrtombs",
  "wcstod",
  "wcstof",
  "wcstol",
  "wcstoll",
  "wcstoul",
  "wcstoull",
  "wctob",
  "wmemchr",
  "wmemcmp",
  "wmemcpy",
  "wmemmove",
  "wmemset",
  "write",
  0 };

DynLibFunction dynlib_functions[] = {
  {"AAsset_close", (uintptr_t)&stub_AAsset_close},
  {"AAsset_getLength", (uintptr_t)&stub_AAsset_getLength},
  {"AAssetManager_fromJava", (uintptr_t)&stub_AAssetManager_fromJava},
  {"AAssetManager_open", (uintptr_t)&stub_AAssetManager_open},
  {"AAsset_read", (uintptr_t)&stub_AAsset_read},
  {"AAsset_seek", (uintptr_t)&stub_AAsset_seek},
  {"abort", 0},
  {"accept", (uintptr_t)&stub_accept},
  {"access", 0},
  {"acos", 0},
  {"acosf", 0},
  {"AHardwareBuffer_acquire", (uintptr_t)&stub_AHardwareBuffer_acquire},
  {"AHardwareBuffer_describe", (uintptr_t)&stub_AHardwareBuffer_describe},
  {"AHardwareBuffer_release", (uintptr_t)&stub_AHardwareBuffer_release},
  {"AImage_delete", (uintptr_t)&stub_AImage_delete},
  {"AImage_deleteAsync", (uintptr_t)&stub_AImage_deleteAsync},
  {"AImage_getHardwareBuffer", (uintptr_t)&stub_AImage_getHardwareBuffer},
  {"AImage_getTimestamp", (uintptr_t)&stub_AImage_getTimestamp},
  {"AImage_getWidth", (uintptr_t)&stub_AImage_getWidth},
  {"AImageReader_acquireLatestImage", (uintptr_t)&stub_AImageReader_acquireLatestImage},
  {"AImageReader_delete", (uintptr_t)&stub_AImageReader_delete},
  {"AImageReader_getWindow", (uintptr_t)&stub_AImageReader_getWindow},
  {"AImageReader_newWithUsage", (uintptr_t)&stub_AImageReader_newWithUsage},
  {"AImageReader_setBufferRemovedListener", (uintptr_t)&stub_AImageReader_setBufferRemovedListener},
  {"AImageReader_setImageListener", (uintptr_t)&stub_AImageReader_setImageListener},
  {"ALooper_acquire", (uintptr_t)&stub_ALooper_acquire},
  {"ALooper_forThread", (uintptr_t)&stub_ALooper_forThread},
  {"ALooper_pollOnce", (uintptr_t)&stub_ALooper_pollOnce},
  {"ALooper_prepare", (uintptr_t)&stub_ALooper_prepare},
  {"ALooper_release", (uintptr_t)&stub_ALooper_release},
  {"ALooper_wake", (uintptr_t)&stub_ALooper_wake},
  {"AMediaCodec_configure", (uintptr_t)&stub_AMediaCodec_configure},
  {"AMediaCodec_createDecoderByType", (uintptr_t)&stub_AMediaCodec_createDecoderByType},
  {"AMediaCodec_delete", (uintptr_t)&stub_AMediaCodec_delete},
  {"AMediaCodec_dequeueInputBuffer", (uintptr_t)&stub_AMediaCodec_dequeueInputBuffer},
  {"AMediaCodec_dequeueOutputBuffer", (uintptr_t)&stub_AMediaCodec_dequeueOutputBuffer},
  {"AMediaCodec_flush", (uintptr_t)&stub_AMediaCodec_flush},
  {"AMediaCodec_getInputBuffer", (uintptr_t)&stub_AMediaCodec_getInputBuffer},
  {"AMediaCodec_getOutputBuffer", (uintptr_t)&stub_AMediaCodec_getOutputBuffer},
  {"AMediaCodec_getOutputFormat", (uintptr_t)&stub_AMediaCodec_getOutputFormat},
  {"AMediaCodec_queueInputBuffer", (uintptr_t)&stub_AMediaCodec_queueInputBuffer},
  {"AMediaCodec_releaseOutputBuffer", (uintptr_t)&stub_AMediaCodec_releaseOutputBuffer},
  {"AMediaCodec_setOutputSurface", (uintptr_t)&stub_AMediaCodec_setOutputSurface},
  {"AMediaCodec_start", (uintptr_t)&stub_AMediaCodec_start},
  {"AMediaCodec_stop", (uintptr_t)&stub_AMediaCodec_stop},
  {"AMediaDataSource_delete", (uintptr_t)&stub_AMediaDataSource_delete},
  {"AMediaDataSource_new", (uintptr_t)&stub_AMediaDataSource_new},
  {"AMediaDataSource_setClose", (uintptr_t)&stub_AMediaDataSource_setClose},
  {"AMediaDataSource_setGetSize", (uintptr_t)&stub_AMediaDataSource_setGetSize},
  {"AMediaDataSource_setReadAt", (uintptr_t)&stub_AMediaDataSource_setReadAt},
  {"AMediaDataSource_setUserdata", (uintptr_t)&stub_AMediaDataSource_setUserdata},
  {"AMediaExtractor_advance", (uintptr_t)&stub_AMediaExtractor_advance},
  {"AMediaExtractor_delete", (uintptr_t)&stub_AMediaExtractor_delete},
  {"AMediaExtractor_getSampleTime", (uintptr_t)&stub_AMediaExtractor_getSampleTime},
  {"AMediaExtractor_getSampleTrackIndex", (uintptr_t)&stub_AMediaExtractor_getSampleTrackIndex},
  {"AMediaExtractor_getTrackCount", (uintptr_t)&stub_AMediaExtractor_getTrackCount},
  {"AMediaExtractor_getTrackFormat", (uintptr_t)&stub_AMediaExtractor_getTrackFormat},
  {"AMediaExtractor_new", (uintptr_t)&stub_AMediaExtractor_new},
  {"AMediaExtractor_readSampleData", (uintptr_t)&stub_AMediaExtractor_readSampleData},
  {"AMediaExtractor_seekTo", (uintptr_t)&stub_AMediaExtractor_seekTo},
  {"AMediaExtractor_selectTrack", (uintptr_t)&stub_AMediaExtractor_selectTrack},
  {"AMediaExtractor_setDataSource", (uintptr_t)&stub_AMediaExtractor_setDataSource},
  {"AMediaExtractor_setDataSourceCustom", (uintptr_t)&stub_AMediaExtractor_setDataSourceCustom},
  {"AMediaExtractor_setDataSourceFd", (uintptr_t)&stub_AMediaExtractor_setDataSourceFd},
  {"AMediaFormat_delete", (uintptr_t)&stub_AMediaFormat_delete},
  {"AMediaFormat_getFloat", (uintptr_t)&stub_AMediaFormat_getFloat},
  {"AMediaFormat_getInt32", (uintptr_t)&stub_AMediaFormat_getInt32},
  {"AMediaFormat_getInt64", (uintptr_t)&stub_AMediaFormat_getInt64},
  {"AMediaFormat_getString", (uintptr_t)&stub_AMediaFormat_getString},
  {"AMEDIAFORMAT_KEY_CHANNEL_COUNT", (uintptr_t)&stub_AMEDIAFORMAT_KEY_CHANNEL_COUNT},
  {"AMEDIAFORMAT_KEY_COLOR_FORMAT", (uintptr_t)&stub_AMEDIAFORMAT_KEY_COLOR_FORMAT},
  {"AMEDIAFORMAT_KEY_COLOR_RANGE", (uintptr_t)&stub_AMEDIAFORMAT_KEY_COLOR_RANGE},
  {"AMEDIAFORMAT_KEY_COLOR_STANDARD", (uintptr_t)&stub_AMEDIAFORMAT_KEY_COLOR_STANDARD},
  {"AMEDIAFORMAT_KEY_DURATION", (uintptr_t)&stub_AMEDIAFORMAT_KEY_DURATION},
  {"AMEDIAFORMAT_KEY_ENCODER_DELAY", (uintptr_t)&stub_AMEDIAFORMAT_KEY_ENCODER_DELAY},
  {"AMEDIAFORMAT_KEY_FRAME_RATE", (uintptr_t)&stub_AMEDIAFORMAT_KEY_FRAME_RATE},
  {"AMEDIAFORMAT_KEY_HEIGHT", (uintptr_t)&stub_AMEDIAFORMAT_KEY_HEIGHT},
  {"AMEDIAFORMAT_KEY_LANGUAGE", (uintptr_t)&stub_AMEDIAFORMAT_KEY_LANGUAGE},
  {"AMEDIAFORMAT_KEY_MIME", (uintptr_t)&stub_AMEDIAFORMAT_KEY_MIME},
  {"AMEDIAFORMAT_KEY_ROTATION", (uintptr_t)&stub_AMEDIAFORMAT_KEY_ROTATION},
  {"AMEDIAFORMAT_KEY_SAMPLE_RATE", (uintptr_t)&stub_AMEDIAFORMAT_KEY_SAMPLE_RATE},
  {"AMEDIAFORMAT_KEY_SLICE_HEIGHT", (uintptr_t)&stub_AMEDIAFORMAT_KEY_SLICE_HEIGHT},
  {"AMEDIAFORMAT_KEY_STRIDE", (uintptr_t)&stub_AMEDIAFORMAT_KEY_STRIDE},
  {"AMEDIAFORMAT_KEY_WIDTH", (uintptr_t)&stub_AMEDIAFORMAT_KEY_WIDTH},
  {"AMediaFormat_setInt32", (uintptr_t)&stub_AMediaFormat_setInt32},
  {"ANativeWindow_acquire", (uintptr_t)&stub_ANativeWindow_acquire},
  {"ANativeWindow_fromSurface", (uintptr_t)&stub_ANativeWindow_fromSurface},
  {"ANativeWindow_getHeight", (uintptr_t)&stub_ANativeWindow_getHeight},
  {"ANativeWindow_getWidth", (uintptr_t)&stub_ANativeWindow_getWidth},
  {"ANativeWindow_release", (uintptr_t)&stub_ANativeWindow_release},
  {"ANativeWindow_setBuffersGeometry", (uintptr_t)&stub_ANativeWindow_setBuffersGeometry},
  {"ANativeWindow_toSurface", (uintptr_t)&stub_ANativeWindow_toSurface},
  {"__android_log_print", (uintptr_t)&stub___android_log_print},
  {"__android_log_vprint", (uintptr_t)&stub___android_log_vprint},
  {"__android_log_write", (uintptr_t)&stub___android_log_write},
  {"android_set_abort_message", (uintptr_t)&stub_android_set_abort_message},
  {"ASensorEventQueue_disableSensor", (uintptr_t)&stub_ASensorEventQueue_disableSensor},
  {"ASensorEventQueue_enableSensor", (uintptr_t)&stub_ASensorEventQueue_enableSensor},
  {"ASensorEventQueue_getEvents", (uintptr_t)&stub_ASensorEventQueue_getEvents},
  {"ASensorEventQueue_hasEvents", (uintptr_t)&stub_ASensorEventQueue_hasEvents},
  {"ASensorEventQueue_setEventRate", (uintptr_t)&stub_ASensorEventQueue_setEventRate},
  {"ASensor_getMinDelay", (uintptr_t)&stub_ASensor_getMinDelay},
  {"ASensor_getName", (uintptr_t)&stub_ASensor_getName},
  {"ASensor_getResolution", (uintptr_t)&stub_ASensor_getResolution},
  {"ASensor_getType", (uintptr_t)&stub_ASensor_getType},
  {"ASensor_getVendor", (uintptr_t)&stub_ASensor_getVendor},
  {"ASensorManager_createEventQueue", (uintptr_t)&stub_ASensorManager_createEventQueue},
  {"ASensorManager_destroyEventQueue", (uintptr_t)&stub_ASensorManager_destroyEventQueue},
  {"ASensorManager_getDefaultSensor", (uintptr_t)&stub_ASensorManager_getDefaultSensor},
  {"ASensorManager_getInstance", (uintptr_t)&stub_ASensorManager_getInstance},
  {"ASensorManager_getSensorList", (uintptr_t)&stub_ASensorManager_getSensorList},
  {"asinf", 0},
  {"atan", 0},
  {"atan2", 0},
  {"atan2f", 0},
  {"atanf", 0},
  {"atof", 0},
  {"atoi", 0},
  {"atol", 0},
  {"basename", (uintptr_t)&stub_basename},
  {"bind", (uintptr_t)&stub_bind},
  {"bsearch", 0},
  {"btowc", 0},
  {"calloc", 0},
  {"chmod", (uintptr_t)&stub_chmod},
  {"clearerr", 0},
  {"clock", 0},
  {"clock_getres", 0},
  {"clock_gettime", 0},
  {"close", 0},
  {"closedir", 0},
  {"closelog", (uintptr_t)&stub_closelog},
  {"connect", (uintptr_t)&stub_connect},
  {"cos", 0},
  {"cosf", 0},
  {"_ctype_", (uintptr_t)&stub__ctype_},
  {"__ctype_get_mb_cur_max", (uintptr_t)&stub___ctype_get_mb_cur_max},
  {"__cxa_atexit", (uintptr_t)&stub___cxa_atexit},
  {"__cxa_finalize", (uintptr_t)&stub___cxa_finalize},
  {"__cxa_pure_virtual", (uintptr_t)&stub___cxa_pure_virtual},
  {"difftime", 0},
  {"div", 0},
  {"dladdr", (uintptr_t)&stub_dladdr},
  {"dlclose", 0},
  {"dlerror", 0},
  {"dl_iterate_phdr", (uintptr_t)&stub_dl_iterate_phdr},
  {"dlopen", 0},
  {"dlsym", 0},
  {"dup", 0},
  {"dup2", 0},
  {"eglChooseConfig", 0},
  {"eglCreateContext", 0},
  {"eglCreatePbufferSurface", 0},
  {"eglCreateWindowSurface", 0},
  {"eglDestroyContext", 0},
  {"eglDestroySurface", 0},
  {"eglGetConfigAttrib", 0},
  {"eglGetCurrentContext", 0},
  {"eglGetCurrentSurface", 0},
  {"eglGetDisplay", 0},
  {"eglGetError", 0},
  {"eglGetProcAddress", 0},
  {"eglInitialize", 0},
  {"eglMakeCurrent", 0},
  {"eglQueryString", 0},
  {"eglQuerySurface", 0},
  {"eglSurfaceAttrib", 0},
  {"eglSwapBuffers", 0},
  {"eglSwapInterval", 0},
  {"eglTerminate", 0},
  {"__errno", (uintptr_t)&stub___errno},
  {"exit", 0},
  {"exp", 0},
  {"exp2f", (uintptr_t)&stub_exp2f},
  {"expf", 0},
  {"fchmod", (uintptr_t)&stub_fchmod},
  {"fclose", 0},
  {"fcntl", 0},
  {"__FD_ISSET_chk", (uintptr_t)&stub___FD_ISSET_chk},
  {"fdopen", 0},
  {"__FD_SET_chk", (uintptr_t)&stub___FD_SET_chk},
  {"feof", 0},
  {"ferror", 0},
  {"fflush", 0},
  {"fgets", 0},
  {"fileno", 0},
  {"flock", (uintptr_t)&stub_flock},
  {"fmod", 0},
  {"fmodf", 0},
  {"fnmatch", (uintptr_t)&stub_fnmatch},
  {"fopen", 0},
  {"fprintf", 0},
  {"fputc", 0},
  {"fputs", 0},
  {"fread", 0},
  {"free", 0},
  {"freeaddrinfo", (uintptr_t)&stub_freeaddrinfo},
  {"freelocale", 0},
  {"fscanf", (uintptr_t)&stub_fscanf},
  {"fseek", 0},
  {"fseeko", 0},
  {"fstat", 0},
  {"ftell", 0},
  {"ftello", 0},
  {"ftruncate", 0},
  {"futimens", (uintptr_t)&stub_futimens},
  {"fwrite", 0},
  {"getaddrinfo", (uintptr_t)&stub_getaddrinfo},
  {"getauxval", 0},
  {"getcwd", 0},
  {"getegid", (uintptr_t)&stub_getegid},
  {"getenv", 0},
  {"geteuid", (uintptr_t)&stub_geteuid},
  {"gethostbyaddr", (uintptr_t)&stub_gethostbyaddr},
  {"gethostbyname", (uintptr_t)&stub_gethostbyname},
  {"gethostname", (uintptr_t)&stub_gethostname},
  {"getnameinfo", (uintptr_t)&stub_getnameinfo},
  {"getpagesize", 0},
  {"getpeername", (uintptr_t)&stub_getpeername},
  {"getpid", 0},
  {"getpriority", (uintptr_t)&stub_getpriority},
  {"getpwuid", (uintptr_t)&stub_getpwuid},
  {"getpwuid_r", (uintptr_t)&stub_getpwuid_r},
  {"getsockname", (uintptr_t)&stub_getsockname},
  {"getsockopt", (uintptr_t)&stub_getsockopt},
  {"gettid", 0},
  {"gettimeofday", 0},
  {"getuid", 0},
  {"gmtime", 0},
  {"gmtime_r", 0},
  {"hypot", 0},
  {"if_nametoindex", (uintptr_t)&stub_if_nametoindex},
  {"inet_addr", (uintptr_t)&stub_inet_addr},
  {"inet_ntop", (uintptr_t)&stub_inet_ntop},
  {"inet_pton", (uintptr_t)&stub_inet_pton},
  {"inflate", (uintptr_t)&stub_inflate},
  {"inflateEnd", (uintptr_t)&stub_inflateEnd},
  {"inflateInit2_", (uintptr_t)&stub_inflateInit2_},
  {"ioctl", 0},
  {"isatty", (uintptr_t)&stub_isatty},
  {"isdigit_l", (uintptr_t)&stub_isdigit_l},
  {"islower_l", (uintptr_t)&stub_islower_l},
  {"isupper_l", (uintptr_t)&stub_isupper_l},
  {"iswalpha_l", (uintptr_t)&stub_iswalpha_l},
  {"iswblank_l", (uintptr_t)&stub_iswblank_l},
  {"iswcntrl_l", (uintptr_t)&stub_iswcntrl_l},
  {"iswdigit_l", (uintptr_t)&stub_iswdigit_l},
  {"iswlower_l", (uintptr_t)&stub_iswlower_l},
  {"iswprint_l", (uintptr_t)&stub_iswprint_l},
  {"iswpunct_l", (uintptr_t)&stub_iswpunct_l},
  {"iswspace_l", (uintptr_t)&stub_iswspace_l},
  {"iswupper_l", (uintptr_t)&stub_iswupper_l},
  {"iswxdigit_l", (uintptr_t)&stub_iswxdigit_l},
  {"isxdigit_l", (uintptr_t)&stub_isxdigit_l},
  {"ldexp", 0},
  {"ldexpf", (uintptr_t)&stub_ldexpf},
  {"link", 0},
  {"listen", (uintptr_t)&stub_listen},
  {"lldiv", (uintptr_t)&stub_lldiv},
  {"localeconv", 0},
  {"localtime", 0},
  {"localtime_r", 0},
  {"log", 0},
  {"log10", 0},
  {"log10f", 0},
  {"log2", 0},
  {"logb", (uintptr_t)&stub_logb},
  {"logf", 0},
  {"longjmp", 0},
  {"lrand48", (uintptr_t)&stub_lrand48},
  {"lseek", 0},
  {"lseek64", 0},
  {"lstat", 0},
  {"madvise", 0},
  {"malloc", 0},
  {"mbrlen", 0},
  {"mbrtowc", 0},
  {"mbsnrtowcs", 0},
  {"mbsrtowcs", (uintptr_t)&stub_mbsrtowcs},
  {"mbtowc", (uintptr_t)&stub_mbtowc},
  {"memalign", (uintptr_t)&stub_memalign},
  {"memchr", 0},
  {"memcmp", 0},
  {"memcpy", 0},
  {"__memcpy_chk", (uintptr_t)&stub___memcpy_chk},
  {"memmove", 0},
  {"__memmove_chk", (uintptr_t)&stub___memmove_chk},
  {"memrchr", 0},
  {"memset", 0},
  {"mkdir", 0},
  {"mktime", 0},
  {"mmap", 0},
  {"modf", 0},
  {"modff", (uintptr_t)&stub_modff},
  {"mprotect", 0},
  {"munmap", 0},
  {"nanosleep", 0},
  {"newlocale", 0},
  {"open", 0},
  {"opendir", 0},
  {"openlog", (uintptr_t)&stub_openlog},
  {"pipe", 0},
  {"poll", 0},
  {"posix_memalign", 0},
  {"pow", 0},
  {"powf", 0},
  {"prctl", 0},
  {"printf", 0},
  {"pthread_atfork", 0},
  {"pthread_attr_destroy", 0},
  {"pthread_attr_getstack", 0},
  {"pthread_attr_init", 0},
  {"pthread_attr_setdetachstate", 0},
  {"pthread_attr_setstacksize", 0},
  {"pthread_condattr_destroy", 0},
  {"pthread_condattr_init", 0},
  {"pthread_condattr_setclock", 0},
  {"pthread_cond_broadcast", 0},
  {"pthread_cond_destroy", 0},
  {"pthread_cond_init", 0},
  {"pthread_cond_signal", 0},
  {"pthread_cond_timedwait", 0},
  {"pthread_cond_wait", 0},
  {"pthread_create", 0},
  {"pthread_detach", 0},
  {"pthread_equal", 0},
  {"pthread_exit", 0},
  {"pthread_getattr_np", 0},
  {"pthread_getspecific", 0},
  {"pthread_join", 0},
  {"pthread_key_create", 0},
  {"pthread_key_delete", 0},
  {"pthread_kill", 0},
  {"pthread_mutexattr_destroy", 0},
  {"pthread_mutexattr_init", 0},
  {"pthread_mutexattr_settype", 0},
  {"pthread_mutex_destroy", 0},
  {"pthread_mutex_init", 0},
  {"pthread_mutex_lock", 0},
  {"pthread_mutex_trylock", 0},
  {"pthread_mutex_unlock", 0},
  {"pthread_once", 0},
  {"pthread_rwlock_init", 0},
  {"pthread_rwlock_rdlock", 0},
  {"pthread_rwlock_unlock", 0},
  {"pthread_rwlock_wrlock", 0},
  {"pthread_self", 0},
  {"pthread_setname_np", 0},
  {"pthread_setspecific", 0},
  {"pthread_sigmask", 0},
  {"ptrace", (uintptr_t)&stub_ptrace},
  {"puts", 0},
  {"qsort", 0},
  {"raise", (uintptr_t)&stub_raise},
  {"rand", 0},
  {"read", 0},
  {"readdir", 0},
  {"readlink", 0},
  {"realloc", 0},
  {"realpath", 0},
  {"recv", (uintptr_t)&stub_recv},
  {"recvfrom", (uintptr_t)&stub_recvfrom},
  {"recvmsg", (uintptr_t)&stub_recvmsg},
  {"remove", 0},
  {"rename", 0},
  {"rewind", 0},
  {"rmdir", 0},
  {"scalbn", (uintptr_t)&stub_scalbn},
  {"sched_getaffinity", (uintptr_t)&stub_sched_getaffinity},
  {"sched_setaffinity", (uintptr_t)&stub_sched_setaffinity},
  {"sched_yield", 0},
  {"select", 0},
  {"sem_destroy", 0},
  {"sem_getvalue", 0},
  {"sem_init", 0},
  {"sem_post", 0},
  {"sem_timedwait", 0},
  {"sem_wait", 0},
  {"send", (uintptr_t)&stub_send},
  {"sendfile", (uintptr_t)&stub_sendfile},
  {"sendmsg", (uintptr_t)&stub_sendmsg},
  {"setbuf", 0},
  {"setenv", 0},
  {"setjmp", 0},
  {"setlocale", 0},
  {"setpriority", (uintptr_t)&stub_setpriority},
  {"setsockopt", (uintptr_t)&stub_setsockopt},
  {"setvbuf", 0},
  {"__sF", (uintptr_t)&stub___sF},
  {"shutdown", (uintptr_t)&stub_shutdown},
  {"sigaction", (uintptr_t)&stub_sigaction},
  {"sigaddset", 0},
  {"sigaltstack", 0},
  {"sigdelset", 0},
  {"sigemptyset", 0},
  {"sigfillset", 0},
  {"signal", (uintptr_t)&stub_signal},
  {"sigsuspend", (uintptr_t)&stub_sigsuspend},
  {"sin", 0},
  {"sincos", (uintptr_t)&stub_sincos},
  {"sincosf", (uintptr_t)&stub_sincosf},
  {"sinf", 0},
  {"slCreateEngine", (uintptr_t)&stub_slCreateEngine},
  {"SL_IID_3DCOMMIT", (uintptr_t)&stub_SL_IID_3DCOMMIT},
  {"SL_IID_3DDOPPLER", (uintptr_t)&stub_SL_IID_3DDOPPLER},
  {"SL_IID_3DGROUPING", (uintptr_t)&stub_SL_IID_3DGROUPING},
  {"SL_IID_3DLOCATION", (uintptr_t)&stub_SL_IID_3DLOCATION},
  {"SL_IID_3DMACROSCOPIC", (uintptr_t)&stub_SL_IID_3DMACROSCOPIC},
  {"SL_IID_3DSOURCE", (uintptr_t)&stub_SL_IID_3DSOURCE},
  {"SL_IID_ANDROIDCONFIGURATION", (uintptr_t)&stub_SL_IID_ANDROIDCONFIGURATION},
  {"SL_IID_ANDROIDEFFECT", (uintptr_t)&stub_SL_IID_ANDROIDEFFECT},
  {"SL_IID_ANDROIDEFFECTCAPABILITIES", (uintptr_t)&stub_SL_IID_ANDROIDEFFECTCAPABILITIES},
  {"SL_IID_ANDROIDEFFECTSEND", (uintptr_t)&stub_SL_IID_ANDROIDEFFECTSEND},
  {"SL_IID_ANDROIDSIMPLEBUFFERQUEUE", (uintptr_t)&stub_SL_IID_ANDROIDSIMPLEBUFFERQUEUE},
  {"SL_IID_AUDIODECODERCAPABILITIES", (uintptr_t)&stub_SL_IID_AUDIODECODERCAPABILITIES},
  {"SL_IID_AUDIOENCODER", (uintptr_t)&stub_SL_IID_AUDIOENCODER},
  {"SL_IID_AUDIOENCODERCAPABILITIES", (uintptr_t)&stub_SL_IID_AUDIOENCODERCAPABILITIES},
  {"SL_IID_AUDIOIODEVICECAPABILITIES", (uintptr_t)&stub_SL_IID_AUDIOIODEVICECAPABILITIES},
  {"SL_IID_BASSBOOST", (uintptr_t)&stub_SL_IID_BASSBOOST},
  {"SL_IID_BUFFERQUEUE", (uintptr_t)&stub_SL_IID_BUFFERQUEUE},
  {"SL_IID_DEVICEVOLUME", (uintptr_t)&stub_SL_IID_DEVICEVOLUME},
  {"SL_IID_DYNAMICINTERFACEMANAGEMENT", (uintptr_t)&stub_SL_IID_DYNAMICINTERFACEMANAGEMENT},
  {"SL_IID_DYNAMICSOURCE", (uintptr_t)&stub_SL_IID_DYNAMICSOURCE},
  {"SL_IID_EFFECTSEND", (uintptr_t)&stub_SL_IID_EFFECTSEND},
  {"SL_IID_ENGINE", (uintptr_t)&stub_SL_IID_ENGINE},
  {"SL_IID_ENGINECAPABILITIES", (uintptr_t)&stub_SL_IID_ENGINECAPABILITIES},
  {"SL_IID_ENVIRONMENTALREVERB", (uintptr_t)&stub_SL_IID_ENVIRONMENTALREVERB},
  {"SL_IID_EQUALIZER", (uintptr_t)&stub_SL_IID_EQUALIZER},
  {"SL_IID_LED", (uintptr_t)&stub_SL_IID_LED},
  {"SL_IID_METADATAEXTRACTION", (uintptr_t)&stub_SL_IID_METADATAEXTRACTION},
  {"SL_IID_METADATATRAVERSAL", (uintptr_t)&stub_SL_IID_METADATATRAVERSAL},
  {"SL_IID_MIDIMESSAGE", (uintptr_t)&stub_SL_IID_MIDIMESSAGE},
  {"SL_IID_MIDIMUTESOLO", (uintptr_t)&stub_SL_IID_MIDIMUTESOLO},
  {"SL_IID_MIDITEMPO", (uintptr_t)&stub_SL_IID_MIDITEMPO},
  {"SL_IID_MIDITIME", (uintptr_t)&stub_SL_IID_MIDITIME},
  {"SL_IID_MUTESOLO", (uintptr_t)&stub_SL_IID_MUTESOLO},
  {"SL_IID_NULL", (uintptr_t)&stub_SL_IID_NULL},
  {"SL_IID_OBJECT", (uintptr_t)&stub_SL_IID_OBJECT},
  {"SL_IID_OUTPUTMIX", (uintptr_t)&stub_SL_IID_OUTPUTMIX},
  {"SL_IID_PITCH", (uintptr_t)&stub_SL_IID_PITCH},
  {"SL_IID_PLAY", (uintptr_t)&stub_SL_IID_PLAY},
  {"SL_IID_PLAYBACKRATE", (uintptr_t)&stub_SL_IID_PLAYBACKRATE},
  {"SL_IID_PREFETCHSTATUS", (uintptr_t)&stub_SL_IID_PREFETCHSTATUS},
  {"SL_IID_PRESETREVERB", (uintptr_t)&stub_SL_IID_PRESETREVERB},
  {"SL_IID_RATEPITCH", (uintptr_t)&stub_SL_IID_RATEPITCH},
  {"SL_IID_RECORD", (uintptr_t)&stub_SL_IID_RECORD},
  {"SL_IID_SEEK", (uintptr_t)&stub_SL_IID_SEEK},
  {"SL_IID_THREADSYNC", (uintptr_t)&stub_SL_IID_THREADSYNC},
  {"SL_IID_VIBRA", (uintptr_t)&stub_SL_IID_VIBRA},
  {"SL_IID_VIRTUALIZER", (uintptr_t)&stub_SL_IID_VIRTUALIZER},
  {"SL_IID_VISUALIZATION", (uintptr_t)&stub_SL_IID_VISUALIZATION},
  {"SL_IID_VOLUME", (uintptr_t)&stub_SL_IID_VOLUME},
  {"snprintf", 0},
  {"socket", (uintptr_t)&stub_socket},
  {"sprintf", 0},
  {"sqrtf", 0},
  {"srand", 0},
  {"srand48", (uintptr_t)&stub_srand48},
  {"sscanf", 0},
  {"__stack_chk_fail", (uintptr_t)&stub___stack_chk_fail},
  {"__stack_chk_guard", (uintptr_t)&stub___stack_chk_guard},
  {"stat", 0},
  {"statfs", 0},
  {"stpcpy", (uintptr_t)&stub_stpcpy},
  {"strcasecmp", 0},
  {"strcasestr", (uintptr_t)&stub_strcasestr},
  {"strcat", 0},
  {"strchr", 0},
  {"strcmp", 0},
  {"strcoll_l", (uintptr_t)&stub_strcoll_l},
  {"strcpy", 0},
  {"strcspn", 0},
  {"strdup", 0},
  {"strerror", 0},
  {"strerror_r", (uintptr_t)&stub_strerror_r},
  {"strftime", 0},
  {"strftime_l", (uintptr_t)&stub_strftime_l},
  {"strlcpy", (uintptr_t)&stub_strlcpy},
  {"strlen", 0},
  {"__strlen_chk", (uintptr_t)&stub___strlen_chk},
  {"strncasecmp", 0},
  {"strncmp", 0},
  {"strncpy", 0},
  {"strnlen", (uintptr_t)&stub_strnlen},
  {"strrchr", 0},
  {"strspn", 0},
  {"strstr", 0},
  {"strtod", 0},
  {"strtof", 0},
  {"strtok_r", (uintptr_t)&stub_strtok_r},
  {"strtol", 0},
  {"strtold", (uintptr_t)&stub_strtold},
  {"strtold_l", (uintptr_t)&stub_strtold_l},
  {"strtoll", 0},
  {"strtoll_l", (uintptr_t)&stub_strtoll_l},
  {"strtoul", 0},
  {"strtoull", 0},
  {"strtoull_l", (uintptr_t)&stub_strtoull_l},
  {"strxfrm_l", (uintptr_t)&stub_strxfrm_l},
  {"swprintf", 0},
  {"symlink", 0},
  {"syscall", (uintptr_t)&stub_syscall},
  {"sysconf", 0},
  {"syslog", (uintptr_t)&stub_syslog},
  {"__system_property_find", (uintptr_t)&stub___system_property_find},
  {"__system_property_get", (uintptr_t)&stub___system_property_get},
  {"__system_property_read", (uintptr_t)&stub___system_property_read},
  {"tan", 0},
  {"tanf", 0},
  {"time", 0},
  {"tolower_l", (uintptr_t)&stub_tolower_l},
  {"toupper", 0},
  {"toupper_l", (uintptr_t)&stub_toupper_l},
  {"towlower", 0},
  {"towlower_l", (uintptr_t)&stub_towlower_l},
  {"towupper_l", (uintptr_t)&stub_towupper_l},
  {"truncate", 0},
  {"uname", (uintptr_t)&stub_uname},
  {"unlink", 0},
  {"unsetenv", 0},
  {"uselocale", 0},
  {"usleep", 0},
  {"utime", (uintptr_t)&stub_utime},
  {"utimes", (uintptr_t)&stub_utimes},
  {"vasprintf", (uintptr_t)&stub_vasprintf},
  {"vfprintf", 0},
  {"vprintf", 0},
  {"vsnprintf", 0},
  {"__vsnprintf_chk", (uintptr_t)&stub___vsnprintf_chk},
  {"__vsprintf_chk", (uintptr_t)&stub___vsprintf_chk},
  {"vsscanf", (uintptr_t)&stub_vsscanf},
  {"wcrtomb", 0},
  {"wcscoll_l", (uintptr_t)&stub_wcscoll_l},
  {"wcslen", 0},
  {"wcsnrtombs", 0},
  {"wcstod", 0},
  {"wcstof", 0},
  {"wcstol", 0},
  {"wcstold", (uintptr_t)&stub_wcstold},
  {"wcstoll", 0},
  {"wcstoul", 0},
  {"wcstoull", 0},
  {"wcsxfrm_l", (uintptr_t)&stub_wcsxfrm_l},
  {"wctob", 0},
  {"wmemchr", 0},
  {"wmemcmp", 0},
  {"wmemcpy", 0},
  {"wmemmove", 0},
  {"wmemset", 0},
  {"write", 0},
  {"_ZdaPv", (uintptr_t)&stub__ZdaPv},
  {"_Znam", (uintptr_t)&stub__Znam},
  {"_ZTH15gDeferredAction", (uintptr_t)&stub__ZTH15gDeferredAction},
};
size_t dynlib_numfunctions = sizeof(dynlib_functions)/sizeof(dynlib_functions[0]);

// resolve os passthrough via dlsym(RTLD_DEFAULT) em runtime
void recon_fill_passthrough(void){
  for(size_t i=0;i<dynlib_numfunctions;i++){
    if(dynlib_functions[i].func==0){
      void *p = dlsym(RTLD_DEFAULT, dynlib_functions[i].symbol);
      if(p) dynlib_functions[i].func=(uintptr_t)p;
      else fprintf(stderr,"[passthrough FALHOU dlsym] %s\\n", dynlib_functions[i].symbol);
    }
  }
}
