BASE=../../../..

LOCAL_INSERT_OBJS=echo recording_session.lo siprec_sdp.lo siprec_metadata.lo \
siprec_invite.lo siprec_media.lo siprec_g711.lo siprec_sb.lo

include $(BASE)/build/modmake.rules
