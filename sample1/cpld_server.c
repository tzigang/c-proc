#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/wait.h>
#include <errno.h>
#include <pthread.h>
#include <malloc.h>

#include "cpld_define.h"
#include "sdh_global.h"

void cpld_accept_connect(int my_type, unsigned int session_id);
void cpld_set_cfg(unsigned char *buff, int length);
void send_connect_ack(unsigned int session_id, int session_req, int result);
void cpld_connect_ack(unsigned int session_id, int session_req, unsigned char *result);
void cpld_connect_close(unsigned int session_id);
void notify_close(unsigned int session_id);
void cpld_reset();
void cpld_set_ip(unsigned char *buff);
void cpld_send_packet(unsigned int session_id, unsigned char *buff, int length);
void cpld_heart_beat();
void cpld_close_session(unsigned int session_id);

extern pthread_mutex_t g_mutex_share_data;
extern CPLD_NET_CFG g_net_cfg;
//cpu类型，1-mcpu，2-ccpu
extern int g_cpu_type;
extern unsigned int g_session_id;
extern CPLD_SESSION_INFO *g_sessions;
extern int g_dev_port;
extern int thread_handle_socket_num;
extern int thread_cpld_proxy_num;
extern int g_net_cfg_ready;
int g_proxy_session_id = 0;
char g_mcpu_netcfg[200] = {0};

#if CPLD_PACKET_FRAG
CPLD_DATA_PACK_NODE *g_packet_node_list = NULL;
#endif


#if CPLD_PACKET_FRAG

/************************************************************
 * 创建一个全新的节点
 *************************************************************/
CPLD_DATA_PACK_NODE *create_packet_node(unsigned int session_id, unsigned int serial, int count)
{
	CPLD_DATA_PACK_NODE *p = (CPLD_DATA_PACK_NODE *)cpld_malloc(sizeof(CPLD_DATA_PACK_NODE));
	p->session_id = session_id;
	p->serial_no = serial;
	p->count = count;
	p->buff = (unsigned char *)cpld_malloc(count * MAX_CPLD_PACKET_LENGTH);
	return p;
}

/************************************************************
 * 链表中加入新的报文碎片
 *************************************************************/
void add_node_to_packet(unsigned int session_id, unsigned char *buff, int length)
{
	unsigned int sn = 0;
	int count = 0;
	int offset = 0;
	//取出参数
	cpld_get_int_from_mem(2, buff, &sn);
	cpld_get_int_from_mem(2, buff + 2, &count);
	cpld_get_int_from_mem(4, buff + 4, &offset);
	
  //链表中找出已有节点
	CPLD_DATA_PACK_NODE *p = g_packet_node_list;
	CPLD_DATA_PACK_NODE *cp = NULL; 
	while(p != NULL){
		if (p->session_id == session_id && p->serial_no == sn){
			cp = p;
			break;
		}
		p = p->next;
	}
	//如果没有找到，创建一个新节点，加入到链表中
	if (cp == NULL){
		cp = create_packet_node(session_id, sn, count);
		if (g_packet_node_list == NULL){
			g_packet_node_list = cp;
		}else{
		  p = g_packet_node_list;
		  while(p->next != NULL){
		  	p = p->next;
		  }
		  p->next = cp;
	  }
	}
	//将数据复制到节点的buff中
	int packet_size = MAX_CPLD_PACKET_LENGTH - CPLD_PACKET_RESTBITS;
	cp->number += 1;
	int p_len = length - CPLD_PACKET_CTRLBITS;
	memcpy(cp->buff + offset, buff + CPLD_PACKET_CTRLBITS, p_len);
	int max_packet_size = MAX_CPLD_PACKET_LENGTH - CPLD_PACKET_RESTBITS;
	if (packet_size * count - (offset + p_len) <= packet_size){ //最后一包
		cp->length = offset + p_len;
	}
	sdh_print_debug("add_node_to_packet---add node:0x%x\n", cp);
}

/************************************************************
 * 返回一个已拼接完整的报文
 *************************************************************/
CPLD_DATA_PACK_NODE *get_hold_packet(unsigned int session_id, unsigned int serial_number)
{
	sdh_print_debug("get_hold_packet input %d, %d\n", session_id, serial_number);
	CPLD_DATA_PACK_NODE *p = g_packet_node_list;
	if (p == NULL) return NULL;
	CPLD_DATA_PACK_NODE *cp = NULL;
	while(p != NULL){
		sdh_print_debug("get_hold_packet---session_id=%d, serial=%d\n",p->session_id,  p->serial_no);
		if (p->session_id == session_id && p->serial_no == serial_number){
			cp = p;
			break;
		}
		p = p->next;
	}
	
	if (cp == NULL) return NULL;
	sdh_print_debug("get_hold_packet find packet %d, %d, %d\n", cp ->length, cp->number, cp->count);
	if (cp->length != 0 && cp->number == cp->count){
		return cp;
	}
	return NULL;
}

/************************************************************
 * 释放一个报文的节点资源
 *************************************************************/
void release_packet(unsigned int session_id, int serial_number)
{
	//链表空时，直接返回
	if (g_packet_node_list == NULL) return;
	CPLD_DATA_PACK_NODE *p = g_packet_node_list;
	CPLD_DATA_PACK_NODE *pp = NULL;
	while(p != NULL){
		if (p->session_id == session_id && p->serial_no == serial_number){
			//查找到相应节点
			if (pp == NULL){
				//第一个节点时
				g_packet_node_list = p->next;
			}else{
			  //多个节点时
			  pp->next = p->next;
			}
			cpld_free(p->buff);
			cpld_free(p);
			break;
		}
		pp = p;
		p = p->next;
	}
}

void release_session_packet(unsigned int session_id)
{
	//链表空时，直接返回
	if (g_packet_node_list == NULL) return;
	CPLD_DATA_PACK_NODE *p = g_packet_node_list;
	CPLD_DATA_PACK_NODE *pp = NULL;
	CPLD_DATA_PACK_NODE *cp = NULL;
	while(p != NULL){
		if (p->session_id == session_id){
			//查找到相应节点
			cp = p->next;
			cpld_free(p->buff);
			cpld_free(p);
			if (pp == NULL){
				//第一个节点时
				g_packet_node_list = cp;
			}else{
			  //多个节点时
			  pp->next = cp;
			}
			p = cp;
			continue;
		}
		pp = p;
		p = p->next;
	}
}

#endif

void *thread_handle_socket(void *arg)
{
  pthread_mutex_lock(&g_mutex_share_data);
  thread_handle_socket_num++;
  pthread_mutex_unlock(&g_mutex_share_data);
	int sock = *(int *)arg;
	cpld_free(arg);
	unsigned char hbuff[10] = {0};
  int my_type = get_cpu_type();
  int r = 1;
	while(r > 0){
		r--;
		bzero(hbuff, 10);
		errno = 0;
	  int rlen = cpld_socket_recv(sock, hbuff, 4, 1);
	  if (rlen <= 0){
		  sdh_print_error("%d: handle_socket: handle_socket(%d): recv error, %d\n", g_cpu_type, sock, errno);
      pthread_mutex_lock(&g_mutex_share_data);
      thread_handle_socket_num--;
      pthread_mutex_unlock(&g_mutex_share_data);
		  return;
	  }
	  //获取报文长度
	  int p_len = 0;
	  cpld_get_int_from_mem(4, hbuff, &p_len);
	  //申请接收报文内存
	  unsigned char *rbuff = (unsigned char *)cpld_malloc(p_len);
	  errno = 0;
	  rlen = cpld_socket_recv(sock, rbuff, p_len, 1);
	  cpld_socket_close(sock);
	  if (rlen <= 0){
	  	sdh_print_error("%d: handle_socket(%d): receive pakcet error %d\n", g_cpu_type, sock, errno);
	  	cpld_free(rbuff);
      pthread_mutex_lock(&g_mutex_share_data);
      thread_handle_socket_num--;
      pthread_mutex_unlock(&g_mutex_share_data);
	  	return;
	  }
	  if (rlen < 12){
	  	sdh_print_error("%d: handle_socket(%d): received packet is not correct, length=%d\n", g_cpu_type, sock, rlen);
	  	cpld_free(rbuff);
      pthread_mutex_lock(&g_mutex_share_data);
      thread_handle_socket_num--;
      pthread_mutex_unlock(&g_mutex_share_data);
	  	return;
	  }

	  int p_type = 0;
	  int session_r = 0;
	  int session_id = 0;
	  int data_length = 0;
	  cpld_get_int_from_mem(2, rbuff + 8, &p_type);
	  cpld_get_int_from_mem(2, rbuff + 10, &session_r);
	  cpld_get_int_from_mem(4, rbuff + 12, &session_id);
	  cpld_get_int_from_mem(4, rbuff + 16, &data_length);
	  int data_offset = 20;
	  switch(p_type){
	  	case CPLD_PACKET_TYPE_CONNECT_REQ:
	  	    //连接请求
	  	    cpld_connect_request(session_id, session_r);
	  	    break;
	  	case CPLD_PACKET_TYPE_CONNECT_ACK:
	  	    //连接应答
	  	    cpld_connect_ack(session_id, session_r, rbuff + data_offset);
	  	    break;
	  	case CPLD_PACKET_TYPE_CONNECT_CLOSE:
	  	    cpld_connect_close(session_id);
	  	    break;
	  	case CPLD_PACKET_TYPE_DATA:
	  	    cpld_send_packet(session_id, rbuff + data_offset, data_length);
	  	    break;
	  	case CPLD_PACKET_TYPE_RESET:
	  	    cpld_reset();
	  	    break;
	  	case CPLD_PACKET_TYPE_SETADDR:
	  	    if (my_type == CPU_TYPE_MCPU)
	  	        cpld_set_ip(rbuff + data_offset);
	  	    break;
	  	case CPLD_PACKET_TYPE_NETCFG_REQ:
	  	    if (my_type == CPU_TYPE_CCPU)
	  	        cpld_get_cfg(session_id);
	  	    break;
	  	case CPLD_PACKET_TYPE_NETCFG_ACK:
	  	    if (my_type == CPU_TYPE_MCPU)
	  	        cpld_set_cfg(rbuff + data_offset, data_length);
	  	    break;
	  	case CPLD_PACKET_TYPE_HEART_BEAT:
	  	    //cpld_heart_beat();
	  	    break;
	  	case CPLD_PACKET_TYPE_CLOSE_SESSION:
	  	    cpld_connect_close(session_id);
	  	    break;
	  	default:
	  	    break;
	  }
	  cpld_free(rbuff);
	  break;
  }
      pthread_mutex_lock(&g_mutex_share_data);
      thread_handle_socket_num--;
      pthread_mutex_unlock(&g_mutex_share_data);
}

void handle_socket(int sock)
{
	pthread_t pthread_client = 0;
	unsigned int *param = (int *)cpld_malloc(sizeof(int));
	memcpy(param, &sock, sizeof(int));
	  pthread_attr_t tattr;
	  int ret = pthread_attr_init(&tattr);
	  ret = pthread_attr_setdetachstate(&tattr, PTHREAD_CREATE_DETACHED);
	ret = pthread_create(&pthread_client, &tattr, thread_handle_socket, (void *)param);
	if (ret != 0){
		cpld_socket_close(sock);
		cpld_free(param);
	}
	return;
}

void *thread_cpld_heart_beat()
{

}

/********************************************
 * MCPU接收到配置以后，开始侦听来自网管中心的连接请求
 *******************************************/
void thread_accept_app(int port)
{
	int server_sock = 0;
	int ret = 0;
  server_sock = cpld_socket_create();
  if (server_sock < 0){
    sdh_print_error("create socket error\n");
 	  return;
  }

  errno = 0;
  ret = cpld_socket_bind(server_sock, port) ;
  if (ret < 0){
    sdh_print_error("pthread_remote_server: bind %d error %d\n", port, errno);
    cpld_socket_close(server_sock);
    sleep(2);
    return;
  }
    //监听socket

  ret = cpld_socket_listen(server_sock);
  if (ret < 0){
    sdh_print_error("listen error \n");
    cpld_socket_close(server_sock);
    sleep(2);
    return;
  }
  
  sdh_print_debug("pthread_remote_server: =====Listening at %d =========\n", port);
  struct sockaddr_in clent;
  for (;;) {
      //接受来自客户端的信息
    int ac_socket = cpld_socket_accept(server_sock);
    if (ac_socket < 0) {
      sdh_print_error("pthread_remote_server: accept error %d \n", ac_socket);
      cpld_socket_close(server_sock);
      break;
    }
    sdh_print_debug("pthread_remote_server: accept connect %d\n", ac_socket);
	  
	  int my_type = get_cpu_type();
	  unsigned int session_id = create_session_id();
	  CPLD_SESSION_INFO *session = (CPLD_SESSION_INFO *)cpld_malloc(sizeof(CPLD_SESSION_INFO));
	  session->session_id = session_id;
	  session->my_type = my_type;
	  session->req_type = my_type;
	  session->peer_sock = ac_socket;
	  time((time_t *)&(session->create_time));
	  insert_session(session);
	  //到对方建立连接这个步骤可以省略
#if HANDL_PTHREAD
    create_proxy_thread(session_id);
#else
    thread_cpld_proxy1(session_id);
#endif
  }
  return;
}

void *pthread_remote_server()
{
	sdh_print_debug("pthread_remote_server enter\n");
	
	int server_port = get_dev1_port();
  thread_accept_app(server_port);
}

void cpld_accept_connect(int my_type, unsigned int session_id)
{
	sdh_print_debug("cpld_accept_connect(%d, %d) enter\n", my_type, session_id);
	
	unsigned char buff[100] = {0};
	
	create_packet_head(CPLD_PACKET_TYPE_CONNECT_REQ, my_type, session_id, buff);
	cpld_set_int_to_mem(4, CPLD_PACKET_HEAD_LENGTH - 4, buff);
	send_to_cpld(buff, CPLD_PACKET_HEAD_LENGTH);
}

int thread_cpld_proxy1(unsigned int session_id)
{
	sdh_print_debug("thread_cpld_proxy1 enter %d\n", session_id);
	int my_type = get_cpu_type();
	char server_ip[32] = {0};
	int server_port = 0;
	char cpu_type[32] = {0};
	
	if (my_type == CPU_TYPE_CCPU){
		strcpy(server_ip, "127.0.0.1");
		server_port = CPLD_SERVER_PORT_CCPU;
		strcpy(cpu_type, "CCPU");
	}else{
	  get_remote_ip(server_ip);
	  server_port = get_remote_port();
	  strcpy(cpu_type, "MCPU");
  }
	sdh_print_debug("thread_cpld_proxy1--%d step 2\n", session_id);
  
	CPLD_SESSION_INFO session;
	bzero(&session, sizeof(CPLD_SESSION_INFO));
  int ret = get_session(session_id, &session);
	if (ret != 0){
		  sdh_print_error("%s: thread_cpld_proxy--------exit 1\n", cpu_type, session_id);
		  return -1;
	}
	sdh_print_debug("thread_cpld_proxy--%d step 3\n", session_id);
	
  //等待连接请求应答
  int sock = session.peer_sock;
	unsigned char hbuff[10] = {0};
	while(1){
		//接收前4个字节
		bzero(hbuff, 10);
		errno = 0;
		int retry = 3;
		int rlen = 0;
		while(retry > 0){
		  rlen = cpld_socket_recv(sock, hbuff, 4, 1);
		  if (rlen < 0){
			  sdh_print_error("%s: thread_cpld_proxy: %d receive packet length error\n", cpu_type, sock);
			  cpld_socket_close(sock);
			  notify_close(session_id);
			  sdh_print_error("%s: thread_cpld_proxy--------exit 2 %d\n", cpu_type, session_id);
			  return -1;
		  }else if (rlen == 0){
			  retry--;
			  sleep(1);
			  if (retry == 0){
			    sdh_print_error("%s: thread_cpld_proxy: %d receive packet length error\n", cpu_type, sock);
			    cpld_socket_close(sock);
			    notify_close(session_id);
			    sdh_print_error("%s: thread_cpld_proxy--------exit 2 %d\n", cpu_type, session_id);
			    return -1;
			  }
		  }else{
		     break;
	    }
	  }
		//获取报文长度值
		int p_len = 0;
		cpld_get_int_from_mem(4, hbuff, &p_len);
		sdh_print_debug("%s: thread_cpld_proxy: packet length is %d\n", cpu_type, p_len);
#if CPLD_PACKET_FRAG
		unsigned char *rbuff = (unsigned char *)cpld_malloc(p_len + CPLD_PACKET_RESTBITS);
		//接收来自socket的报文数据，数据从cpld报文头+4字节开始
		errno = 0;
		rlen = cpld_socket_recv(sock, rbuff + 4, p_len, 1);
		sdh_print_debug("thread_cpld_proxy: %d receive packet %d/%d\n", sock, rlen, p_len);
		if (rlen <= 0){
			sdh_print_error("%s: thread_cpld_proxy: %d receive packet error\n", cpu_type, sock);
			cpld_free(rbuff);
			cpld_socket_close(sock);
			notify_close(session_id);
			sdh_print_error("%s: thread_cpld_proxy--------exit 3 %d\n", cpu_type, session_id);
			return -1;
		}
		//复原socket报文
		memcpy(rbuff, hbuff, 4);
		p_len = p_len + 4;  //报文总长度还要加上报文的长度字节
		//每片报文长度为最大长度-冗余字节
    int p_size = MAX_CPLD_PACKET_LENGTH - CPLD_PACKET_RESTBITS;
    //报文片数
    int count = p_len / p_size;
    //如果不是整数，则加1
    if (p_len % p_size > 0) 
      count = count + 1;
    //报文号是随机数
    int serial_no = rand() % 0x7fff;
    int ptr = 0;
    unsigned char sendbuff[MAX_CPLD_PACKET_LENGTH] = {0};
    create_packet_head(CPLD_PACKET_TYPE_DATA, session.req_type, session_id, sendbuff);
    
    while(p_len - ptr > 0){
    	sdh_print_debug("send packet: from %d, total %d, count=%d\n", ptr, p_len, count);
    	bzero(sendbuff, MAX_CPLD_PACKET_LENGTH);
    	create_packet_head(CPLD_PACKET_TYPE_DATA, session.req_type, session_id, sendbuff);
    	int len = (p_len - ptr > p_size) ? p_size:(p_len - ptr);
    	//设置报文长度
    	cpld_set_int_to_mem(4, len + 8, sendbuff + CPLD_PACKET_HEAD_LENGTH - 4);
    	int p1 = CPLD_PACKET_HEAD_LENGTH;
    	//设置报文控制数据
    	cpld_set_int_to_mem(2, serial_no, sendbuff + p1);
    	cpld_set_int_to_mem(2, count, sendbuff + p1 + 2);
    	cpld_set_int_to_mem(4, ptr, sendbuff + p1 + 4);
    	//复制报文数据
    	memcpy(sendbuff + p1 + 8, rbuff + ptr, len);
    	//设置报文总长度
    	cpld_set_int_to_mem(4, len + 8 + CPLD_PACKET_HEAD_LENGTH - 4, sendbuff);
    	//发送
    	send_to_cpld(sendbuff, p1 + 8 + len); 
    	ptr += len;
    	sdh_print_debug("sended packet: length=%d, next offset is %d\n", len, ptr);
    } 
    cpld_free(rbuff);
#else
		//根据报文长度申请内存
		//整个内存空间包括CPLD报文头
		unsigned char *rbuff = (unsigned char *)cpld_malloc(p_len + 80 + CPLD_PACKET_HEAD_LENGTH);
		//接收来自socket的报文数据，数据从cpld报文头+4字节开始
		errno = 0;
		rlen = cpld_socket_recv(sock, rbuff + 4 + CPLD_PACKET_HEAD_LENGTH, p_len, 1);
		sdh_print_debug("thread_cpld_proxy: %d receive packet %d/%d\n", sock, rlen, p_len);
		if (rlen <= 0){
			sdh_print_error("%s: thread_cpld_proxy: %d receive packet error\n", cpu_type, sock);
			cpld_free(rbuff);
			cpld_socket_close(sock);
			notify_close(session_id);
			sdh_print_error("%s: thread_cpld_proxy--------exit 3 %d\n", cpu_type, session_id);
			return -1;
		}
		//复原socket报文，将报文数据长度放入报文数据的首4个字节
		memcpy(rbuff + CPLD_PACKET_HEAD_LENGTH, hbuff, 4);
		//构建报文头
		create_packet_head(CPLD_PACKET_TYPE_DATA, session.req_type, session_id, rbuff);
		//设置报文数据长度，放入CPLD报文头的最后4个字节
		cpld_set_int_to_mem(4, rlen + 4, rbuff + CPLD_PACKET_HEAD_LENGTH - 4);
		//设置总报文长度，放入CPLD报文头的头4个字节
		int t_len =  rlen + CPLD_PACKET_HEAD_LENGTH;
		cpld_set_int_to_mem(4, t_len, rbuff);
		//通过CPLD发送到对端
		sdh_print_debug("connect to peer app ok, send to peer cpld\n");
	  send_to_cpld(rbuff, t_len + 4); 
	  cpld_free(rbuff);
#endif
 	}
	sdh_print_debug("%s: thread_cpld_proxy--------exit 4 %d\n", cpu_type, session_id);
	return 0;
}

void *thread_cpld_proxy(void *arg)
{
	unsigned int temp = 0;
	unsigned int session_id = *((unsigned int *)arg);
	cpld_free(arg);
	sdh_print_debug("thread_cpld_proxy enter %d\n", session_id);
	thread_cpld_proxy1(session_id);
	return 0;
}

int create_proxy_thread(unsigned int session_id)
{
	pthread_t pthread_client = 0;
	unsigned int *param = (unsigned int *)cpld_malloc(sizeof(unsigned int));

	*param = session_id;
	
	pthread_attr_t tattr;
	int ret = pthread_attr_init(&tattr);
	ret = pthread_attr_setdetachstate(&tattr, PTHREAD_CREATE_DETACHED);
	ret = pthread_create(&pthread_client, &tattr, thread_cpld_proxy, (void *)param);
	sdh_print_debug("create_proxy_thread create thread ret =%d\n", ret);
	if (ret != 0){
		cpld_free(param);
	}
	return ret;
}

int cpld_get_cfg(unsigned int session_id)
{
	char dev_ip[100] = {0};
	char svr_id[100] = {0};
	CPLD_NET_CFG cfg;
	memset(&cfg, 0, sizeof(CPLD_NET_CFG));
	cpld_get_mcpu_netparam(cfg.peer_ip, &cfg.peer_port, &cfg.my_port);
	unsigned char buff[100] = {0};
	int p_len = create_netcfg_ack(session_id, &cfg, buff);
	int rlen = send_to_cpld(buff, p_len);
	if (rlen > 0)
	  return 0;
	else
	  return -1;
}

void cpld_set_cfg(unsigned char *buff, int length)
{
	sdh_print_debug("cpld_set_cfg enter\n");
	int old_dev_port = 0;
	int dev_port = 0;
	int cur_sock = 0;
	pthread_mutex_lock(&g_mutex_share_data);
	g_net_cfg_ready = 1;
	old_dev_port = g_net_cfg.my_port;
	memcpy(g_net_cfg.peer_ip, buff, 16);
	cpld_get_int_from_mem(4, buff + 16, &g_net_cfg.peer_port);
	cpld_get_int_from_mem(4, buff + 20, &g_net_cfg.my_port);
	dev_port = g_net_cfg.my_port;
	cur_sock = g_net_cfg.socket;
	pthread_mutex_unlock(&g_mutex_share_data);
	sdh_print_debug("cpld_set_cfg: new port is %d, old port is %d\n", dev_port, old_dev_port);
	if (dev_port != old_dev_port){
		//关闭原有侦听管理中心的线程
		sdh_print_debug("cpld_set_cfg: close old thread %d\n", cur_sock);
		if (cur_sock > 0){
			cpld_socket_close(cur_sock);
	    pthread_mutex_lock(&g_mutex_share_data);
			g_net_cfg.socket = 0;
	    pthread_mutex_unlock(&g_mutex_share_data);
			sleep(1);
		}
		
		pthread_t thread_id;
	  pthread_attr_t tattr;
	  int ret = pthread_attr_init(&tattr);
	  ret = pthread_attr_setdetachstate(&tattr, PTHREAD_CREATE_DETACHED);
		ret = pthread_create(&thread_id, &tattr, pthread_remote_server, NULL);
	  if (0 != ret)
	  {
		  sdh_print_error("cpld_set_cfg: pthread_remote_server create failed");
		  return;
	  }
	  sdh_print_debug("cpld_set_cfg: pthread_remote_server create ok\n");
	  
	}
	sdh_print_debug("cpld_set_cfg: return\n");

}

int cpld_connect_request(unsigned int session_id, int session_r)
{
	int my_type = get_cpu_type();
	char server_ip[32] = {0};
	int server_port = 0;
	
	if (my_type == CPU_TYPE_CCPU){
		strcpy(server_ip, "127.0.0.1");
		server_port = g_dev_port;
	}else{
	  get_remote_ip(server_ip);
	  server_port = get_remote_port();
  }
  
	struct sockaddr_in sin_addr;
	bzero((unsigned char *)&sin_addr, sizeof(struct sockaddr_in));
	
	int sock = cpld_socket_create();
	if (sock < 0){
		sdh_print_error("cpld_connect_request: create client socket error\n");
    send_close_session(session_id, 0);
		return -1;
	}
  sdh_print_debug("cpld_connect_request: create socket is %d\n", sock);
	int ret = cpld_socket_connect(sock, server_ip, server_port);   //连接服务端
	if (ret != 0){
		sdh_print_error("cpld_connect_request: : cannot connect to %s\n", server_ip);
		cpld_socket_close(sock);
    send_close_session(session_id, 0);
		return -1;
	}
	sdh_print_debug("cpld_connect_request: : connect to %s:%d ok \n", server_ip, server_port);

	CPLD_SESSION_INFO *session = (CPLD_SESSION_INFO *)cpld_malloc(sizeof(CPLD_SESSION_INFO));
	session->flag = 1;
	session->my_type = get_cpu_type();
	session->session_id = session_id;
	session->req_type = session_r;
	session->peer_sock = sock;
	time((time_t *)&(session->create_time));
	insert_session(session);
  int s_id = 0;
  pthread_mutex_lock(&g_mutex_share_data);
  s_id = g_proxy_session_id;
  pthread_mutex_unlock(&g_mutex_share_data);
  if (s_id != 0){
  	remove_session(s_id);
  	sleep(1);
  }
  pthread_mutex_lock(&g_mutex_share_data);
  g_proxy_session_id = session_id;
  pthread_mutex_unlock(&g_mutex_share_data);

	sdh_print_debug("cpld_connect_request: return\n");
	return 0;
}

void send_connect_ack(unsigned int session_id, int session_req, int result)
{
	unsigned char buff[100] = {0};
	//构建报文头
	create_packet_head(CPLD_PACKET_TYPE_CONNECT_ACK, session_req, session_id, buff);
	//设置报文数据长度
	cpld_set_int_to_mem(4, 1, buff + CPLD_PACKET_HEAD_LENGTH - 4);
	//设置连接结果
	buff[CPLD_PACKET_HEAD_LENGTH] = (unsigned char)(result & 0xff);
	//设置总报文长度
	cpld_set_int_to_mem(4, CPLD_PACKET_HEAD_LENGTH + 1 - 4, buff);
	//发送到CPLD通道
	send_to_cpld(buff, CPLD_PACKET_HEAD_LENGTH + 1);
}

void send_close_session(unsigned int session_id, int session_req)
{
	unsigned char buff[100] = {0};
	//构建报文头
	create_packet_head(CPLD_PACKET_TYPE_CLOSE_SESSION, session_req, session_id, buff);
	//设置总报文长度
	cpld_set_int_to_mem(4, CPLD_PACKET_HEAD_LENGTH - 4, buff);
	//发送到CPLD通道
	send_to_cpld(buff, CPLD_PACKET_HEAD_LENGTH);
}

void cpld_send_session_id(unsigned int session_id)
{
	int c_sock = cpld_socket_create();
  if (c_sock <= 0){
  	sdh_print_error("cpld_send_session_id====create socket %d error\n", c_sock);
  	return;
  }
  int port = 0;
  int my_type = get_cpu_type();
  if (my_type == CPU_TYPE_CCPU){
  	port = CCPU_PROXY_PORT;
  }else{
    port = MCPU_PROXY_PORT;
  }
  unsigned char sbuff[100] = {0};
  cpld_set_int_to_mem(4, session_id, sbuff);
  int ret = cpld_socket_connect(c_sock, "127.0.0.1", port);
  if (ret < 0){
  	sdh_print_error("cpld_send_session_id====connect to %d error\n", port);
  	cpld_socket_close(c_sock);
  	return;
  }
  ret = cpld_socket_send(c_sock, sbuff, 4);
  if (ret < 0){
  	sdh_print_error("cpld_send_session_id====send session_id error\n");
  	cpld_socket_close(c_sock);
  	return;
  }
  sdh_print_debug("cpld_send_session_id====send session_id ok\n");
  cpld_socket_close(c_sock);

}

void cpld_connect_ack(unsigned int session_id, int session_req, unsigned char *result)
{

	int ret = (int)result[0];
	if (ret != 0){
		remove_session(session_id);
		return;
	}
	set_session_flag(session_id, 1);
	//创建代理线程
#if HANDL_PTHREAD	
	ret = create_proxy_thread(session_id);
	if (0 != ret)
	{
		sdh_print_error("cpld_connect_ack: Thread thread_cpld_proxy create failed\n");
		//删除相关会话记录
		send_close_session(session_id, session_req);
		remove_session(session_id);
		return;
  }
#endif  
	sdh_print_debug("cpld_connect_ack:return\n");
}

void cpld_connect_close(unsigned int session_id)
{
	int ret = find_session(session_id);
	if (ret == 1){
		remove_session(session_id);
	}
}

void notify_close(unsigned int session_id)
{

	//如果session表中有记录，则表示socket关闭由对方进程产生需要通知另一个CPU
	send_close_session(session_id, 0);
	remove_session(session_id);
}

void cpld_reset()
{
	clear_session_link();
}

void cpld_send_setip_ack(unsigned int session_id, int s_req, int result)
{
	unsigned char sbuff[100] = {0};
	create_packet_head(CPLD_PACKET_TYPE_SETADDR_ACK, s_req, session_id, sbuff);
	cpld_set_int_to_mem(4, CPLD_PACKET_HEAD_LENGTH + 1, sbuff);
	cpld_set_int_to_mem(4, 1, sbuff + CPLD_PACKET_HEAD_LENGTH - 4);
	memset(sbuff + CPLD_PACKET_HEAD_LENGTH, result, 1);
	send_to_cpld(sbuff, 25);
}

void mcpu_send_ip(unsigned int session_id)
{
	unsigned char out[200] = {0};
  create_packet_head(CPLD_PACKET_TYPE_MCPU_NETCFG, CPU_TYPE_MCPU, session_id, out);
  cpld_set_int_to_mem(4, 68, out + CPLD_PACKET_HEAD_LENGTH - 4);
  memcpy(out + CPLD_PACKET_HEAD_LENGTH, g_mcpu_netcfg, 68);
  cpld_set_int_to_mem(4, 68 + CPLD_PACKET_HEAD_LENGTH, out);
  send_to_cpld(out, 68 + CPLD_PACKET_HEAD_LENGTH);
}

void ccpu_save_netcfg(unsigned char *data, int length)
{
	FILE *fp = fopen("/rootfs/neg/peer_net", "w+");
	if (fp == NULL)
	{
		sdh_print_error("cannot open /rootfs/neg/peer_net\n");
		return;
	}
	
	int wlen = fwrite(data, sizeof(unsigned char), length, fp);
	if (wlen <= 0){
		sdh_print_error("write data in /rootfs/neg/peer_net error\n");
		fclose(fp);
		return;
	}
	fclose(fp);
}
/***************************
 *设置MCPU的IP地址，格式为
 16字节IP地址（xxx.xxx.xxx.xxx格式）
 +16字节掩码
 +16字节网关
 +20字节MAC地址(xx:xx:xx:xx:xx:xx)
 **************************/
void cpld_set_ip(unsigned char *buff)
{
	char ip[64] = {0};
	char mask[64] = {0};
	char gw[64] = {0};
	char hwaddr[64] = {0};
	char ifname[64] = {0};
	char ip_str[256] = {0};
	char gw_str[256] = {0};
  get_local_ip(ip, mask, hwaddr, ifname);
	if (buff[0] != 0){
		bzero(ip, 64);
		memcpy(ip, buff, 16);
  }
	
	if (buff[16] != 0){
		bzero(mask, 64);
		memcpy(mask, buff + 16, 16);
	}
	
	int has_gw = 0;
	if (buff[32] != 0){
		has_gw = 1;
		bzero(gw, 64);
		memcpy(gw, buff + 32, 16);
  }
	
	if (buff[48] != 0){
		bzero(hwaddr, 64);
		memcpy(hwaddr, buff + 48, 20);
	}
	
	sprintf(ip_str, "ifconfig eth0 %s netmask %s hw ether %s", ip, mask, hwaddr);
	if (has_gw == 1){
		sprintf(gw_str, "route add default gw %s", gw);
	}
	update_start_file2(ip_str, gw_str);

	return;
}

/*********************************************
 * 收到来自CPLD的数据包
 * CCPU卡需要发送到Listen，MCPU卡需要发送到网管中心
 **********************************************/
void cpld_send_packet(unsigned int session_id, unsigned char *buff, int length)
{
	CPLD_SESSION_INFO session;
	bzero(&session, sizeof(CPLD_SESSION_INFO));
	int i = 0;
	
	int ret = get_session(session_id, &session);
	if (ret != 0){
		sdh_print_error("cpld_send_packet: cannot find session info, %d\n", session_id);
		if (get_cpu_type() == CPU_TYPE_MCPU)
	      cpld_connect_request(session_id, CPU_TYPE_CCPU);
	  else
	      cpld_connect_request(session_id, CPU_TYPE_MCPU);
	  ret = get_session(session_id, &session);
	  if (ret != 0){
	  	sdh_print_error("cpld_send_packet: cannot find session %d info again\n", session_id);
	  	return;
	  }
	}

#if CPLD_PACKET_FRAG	//1 分包，0 不分包
   //如果分包 报文号2+总碎片个数2 +偏移量4
   sdh_print_debug("--------------cpld_send_packet:1----------\n");
   int serial_number = 0;
   int offset = 0;
   int count = 0;
   cpld_get_int_from_mem(2, buff, &serial_number);
   cpld_get_int_from_mem(2, buff + 2, &count);
   cpld_get_int_from_mem(4, buff + 4, &offset);
   sdh_print_debug("cpld_send_packet:1----sno=%d,count=%d,offset=%d\n", serial_number, count, offset);
   if (1 == count){
   	 //如果是一个包，直接发送
   	 sdh_print_debug("this is a single packet, send to sock %d, length = %d\n", session.peer_sock, length - CPLD_PACKET_CTRLBITS);
	     ret = send(session.peer_sock, buff + CPLD_PACKET_CTRLBITS, length - CPLD_PACKET_CTRLBITS, 0);
	     if (ret <= 0){
		     sdh_print_error("cpld_send_packet: send packet error\n");
		     send_close_session(session_id, 0);
		     return;
       }
       sdh_print_debug("send to socket ok!!\n");
   }else{
       //如果是多包，则拼包
     add_node_to_packet(session_id, buff, length);
     //如果拼包完成，发送
     CPLD_DATA_PACK_NODE *send_pack = get_hold_packet(session_id, serial_number);
     sdh_print_debug("get_hold_packet return %d\n", send_pack);
     if (send_pack != NULL){
	     sdh_print_debug("cpld_send_packet--socket %d send %d:\n", session.peer_sock, send_pack->length);
	     print_mem_p(send_pack->buff, send_pack->length);
       ret = send(session.peer_sock, send_pack->buff, send_pack->length, 0);
       release_packet(session_id, serial_number);
       if (ret <= 0){
         sdh_print_error("cpld_send_packet: send packet error\n");
		     send_close_session(session_id, 0);
		     return;
		   }
		   sdh_print_debug("send to sock %d ok\n", session.peer_sock);
     }
   }
#else
	ret = send(session.peer_sock, buff, length, 0);
	if (ret <= 0){
		sdh_print_error("cpld_send_packet: send packet error\n");
		send_close_session(session_id, 0);
		return;
  }
#endif  
}

/**********************************************
 * 线程：侦听来自网管执行链接
 **********************************************/
void *pthread_remote_xcpu_server(void *arg)
{
	int session_id = 0;
	int sock = 0;
	int ret = 0;
  CPLD_SESSION_INFO session;
  unsigned char hbuff[32] = {0};
	while(1){
		pthread_mutex_lock(&g_mutex_share_data);
		session_id = g_proxy_session_id;
		pthread_mutex_unlock(&g_mutex_share_data);
		
		
		if (session_id != 0){
			bzero(&session, sizeof(CPLD_SESSION_INFO));
		  ret = get_session(session_id, &session);
		  if (ret != 0){
			  sdh_print_error("pthread_remote_xcpu_server---no session\n");
		    pthread_mutex_lock(&g_mutex_share_data);
		    g_proxy_session_id = 0;
		    pthread_mutex_unlock(&g_mutex_share_data);
		    continue;
		  }
		  
		  sock = session.peer_sock;
			while(1){
				bzero(hbuff, 32);
				int rlen = cpld_socket_recv(sock, hbuff, 4, 1);
				if (rlen <= 0){
					sdh_print_error("receive from app socket 1 error\n");
					cpld_socket_close(sock);
					pthread_mutex_lock(&g_mutex_share_data);
					g_proxy_session_id = 0;
					pthread_mutex_unlock(&g_mutex_share_data);
					send_close_session(session_id, 0);
					break;
				}
				sdh_print_debug("recv from socket %d\n", sock);
				int p_len = 0;
				cpld_get_int_from_mem(4, hbuff, &p_len);
#if CPLD_PACKET_FRAG
		unsigned char *rbuff = (unsigned char *)cpld_malloc(p_len + CPLD_PACKET_RESTBITS);
		//接收来自socket的报文数据，数据从cpld报文头+4字节开始
		errno = 0;
		rlen = cpld_socket_recv(sock, rbuff + 4, p_len, 1);
		sdh_print_debug("pthread_remote_xcpu_server: %d receive packet %d/%d\n", sock, rlen, p_len);
		if (rlen <= 0){
			sdh_print_error("pthread_remote_xcpu_server: %d receive packet error\n", sock);
			cpld_free(rbuff);
			cpld_socket_close(sock);
			notify_close(session_id);
			sdh_print_error("pthread_remote_xcpu_server--------exit 3 %d\n", session_id);
			return -1;
		}
		//复原socket报文
		memcpy(rbuff, hbuff, 4);
		p_len = p_len + 4;  //报文总长度还要加上报文的长度字节
		//每片报文长度为最大长度-冗余字节
    int p_size = MAX_CPLD_PACKET_LENGTH - CPLD_PACKET_RESTBITS;
    //报文片数
    int count = p_len / p_size;
    //如果不是整数，则加1
    if (p_len % p_size > 0) 
      count = count + 1;
    //报文号是随机数
    int serial_no = rand() % 0x7fff;
    int ptr = 0;
    unsigned char sendbuff[MAX_CPLD_PACKET_LENGTH] = {0};
    create_packet_head(CPLD_PACKET_TYPE_DATA, session.req_type, session_id, sendbuff);
    
    while(p_len - ptr > 0){
    	sdh_print_debug("send packet: from %d, total %d, count=%d\n", ptr, p_len, count);
    	bzero(sendbuff, MAX_CPLD_PACKET_LENGTH);
    	create_packet_head(CPLD_PACKET_TYPE_DATA, session.req_type, session_id, sendbuff);
    	int len = (p_len - ptr > p_size) ? p_size:(p_len - ptr);
    	//设置报文长度
    	cpld_set_int_to_mem(4, len + 8, sendbuff + CPLD_PACKET_HEAD_LENGTH - 4);
    	int p1 = CPLD_PACKET_HEAD_LENGTH;
    	//设置报文控制数据
    	cpld_set_int_to_mem(2, serial_no, sendbuff + p1);
    	cpld_set_int_to_mem(2, count, sendbuff + p1 + 2);
    	cpld_set_int_to_mem(4, ptr, sendbuff + p1 + 4);
    	//复制报文数据
    	memcpy(sendbuff + p1 + 8, rbuff + ptr, len);
    	//设置报文总长度
    	cpld_set_int_to_mem(4, len + 8 + CPLD_PACKET_HEAD_LENGTH - 4, sendbuff);
    	//发送
    	send_to_cpld(sendbuff, p1 + 8 + len); 
    	ptr += len;
    	sdh_print_debug("sended packet: length=%d, next offset is %d\n", len, ptr);
    } 
    cpld_free(rbuff);
#else

				unsigned char *rbuff = (unsigned char *)cpld_malloc(p_len + 8 + CPLD_PACKET_HEAD_LENGTH);
				rlen = cpld_socket_recv(sock, rbuff + 4 + CPLD_PACKET_HEAD_LENGTH, p_len, 1);
				if (rlen <= 0){
					sdh_print_error("receive from app socket 2 error\n");
					cpld_socket_close(sock);
					cpld_free(rbuff);
					pthread_mutex_lock(&g_mutex_share_data);
					g_proxy_session_id = 0;
					pthread_mutex_unlock(&g_mutex_share_data);
					send_close_session(session_id, 0);
        }	
        
		    //复原socket报文，将报文数据长度放入报文数据的首4个字节
		    memcpy(rbuff + CPLD_PACKET_HEAD_LENGTH, hbuff, 4);
		    //构建报文头
		    create_packet_head(CPLD_PACKET_TYPE_DATA, session.req_type, session_id, rbuff);
		    //设置报文数据长度，放入CPLD报文头的最后4个字节
		    cpld_set_int_to_mem(4, rlen + 4, rbuff + CPLD_PACKET_HEAD_LENGTH - 4);
		    //设置总报文长度，放入CPLD报文头的头4个字节
		    int t_len =  rlen + CPLD_PACKET_HEAD_LENGTH;
		    cpld_set_int_to_mem(4, t_len, rbuff);
				send_to_cpld(rbuff, t_len + 4);
				cpld_free(rbuff);
#endif
		    sleep(1);
			}
	  }
  }
}


void *pthread_remote_xcpu_server2(void *arg)
{
  int my_type = get_cpu_type();
  int port = 0;
  if (my_type == CPU_TYPE_MCPU){
  	port = MCPU_PROXY_PORT;
  }else{
    port = CCPU_PROXY_PORT;
  }
	int server_sock = 0;
	int ret = 0;
	while(1){
  server_sock = cpld_socket_create();
  if (server_sock < 0){
    sdh_print_error("create socket error\n");
 	  return;
  }

  errno = 0;
  ret = cpld_socket_bind(server_sock, port) ;
  if (ret < 0){
    sdh_print_error("pthread_remote_ccpu_server: bind %d error %d\n", port, errno);
    cpld_socket_close(server_sock);
    sleep(2);
    return;
  }
    //监听socket
  ret = cpld_socket_listen(server_sock);
  if (ret < 0){
    sdh_print_error("listen error \n");
    cpld_socket_close(server_sock);
    sleep(2);
    return;
  }
  
  sdh_print_debug("pthread_remote_xcpu_server: =====Listening at %d =========\n", port);
  struct sockaddr_in clent;
  for (;;) {
      //接受来自客户端的信息
    int ac_socket = cpld_socket_accept(server_sock);
    if (ac_socket < 0) {
      sdh_print_error("pthread_remote_xcpu_server: accept error %d \n", ac_socket);
      cpld_socket_close(server_sock);
      break;
    }
    sdh_print_debug("pthread_remote_xcpu_server: accept connect %d\n", ac_socket);
    
    unsigned char rbuff[100] = {0};
    ret = recv(ac_socket, rbuff, 100, 0);
    if (ret <= 0){
    	sdh_print_error("pthread_remote_xcpu_server----recv session_id error ret=%d\n", ret);
    	cpld_socket_close(ac_socket);
    	continue;
    }
    
    unsigned int session_id = 0;
    cpld_get_int_from_mem(4, rbuff, &session_id);	
    cpld_socket_close(ac_socket);
    
    thread_cpld_proxy1(session_id);  
  }
  }

  return;
	
}

void init_listen_thread()
{
	pthread_t thread_id;
  pthread_attr_t tattr;
  int ret = pthread_attr_init(&tattr);
  ret = pthread_attr_setdetachstate(&tattr, PTHREAD_CREATE_DETACHED);
	ret = pthread_create(&thread_id, &tattr, pthread_remote_xcpu_server, NULL);
  if (0 != ret)
  {
	  sdh_print_error("cpld_set_cfg: pthread_remote_server create failed");
	  return;
  }
}

