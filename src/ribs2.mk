TARGET=ribs2.a

SRC=context.c epoll_worker.c ctx_pool.c http_server.c hashtable.c mime_types.c http_client_pool.c timeout_handler.c ribify.c logger.c daemonize.c http_headers.c http_cookies.c file_mapper.c ds_var_field.c file_utils.c lhashtable.c search.c json.c memalloc.c mempool.c sleep.c timer.c ringbuf.c ringfile.c sendemail.c ds_loader.c heap.c vmallocator.c base64.c http_file_server.c http_vhost.c apple.c web_server.c tcp_server.c websocket_server.c websocket_log.c

ASM=context_asm.S

CFLAGS+= -I ../include

include ../make/ribs.mk
