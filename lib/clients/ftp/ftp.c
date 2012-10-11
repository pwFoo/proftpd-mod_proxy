/*
 * ProFTPD - mod_proxy FTP client library
 * Copyright (c) 2012 TJ Saunders <tj@castaglia.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Suite 500, Boston, MA 02110-1335, USA.
 *
 * As a special exemption, TJ Saunders and other respective copyright holders
 * give permission to link this program with OpenSSL, and distribute the
 * resulting executable, without including the source code for OpenSSL in the
 * source distribution.
 */

#include "ftp.h"

unsigned char is_master = TRUE;
pid_t mpid = 0;
session_t session;
module *static_modules[] = { NULL };
module *loaded_modules = NULL;

volatile unsigned int recvd_signal_flags = 0;

static int connect_timeout_reached = FALSE;
static pr_netio_stream_t *connect_timeout_strm = NULL;

static int connect_timeout_cb(CALLBACK_FRAME) {
  connect_timeout_reached = TRUE;

  if (connect_timeout_strm != NULL) {
    /* Abort the stream. */
    pr_netio_abort(connect_timeout_strm);
    connect_timeout_strm->strm_errno = ETIMEDOUT;
  }

  return 0;
}

static pr_buffer_t *netio_buffer_alloc(pr_netio_stream_t *nstrm) {
  size_t bufsz;
  pr_buffer_t *pbuf = NULL;

  pbuf = pcalloc(nstrm->strm_pool, sizeof(pr_buffer_t));

  /* Allocate a buffer. */
  bufsz = pr_config_get_server_xfer_bufsz(nstrm->strm_mode);
  pbuf->buf = pcalloc(nstrm->strm_pool, bufsz);
  pbuf->buflen = bufsz;

  /* Position the offset at the start of the buffer, and set the
   * remaining bytes value accordingly.
   */
  pbuf->current = pbuf->buf;
  pbuf->remaining = bufsz;

  /* Add this buffer to the given stream. */
  nstrm->strm_buf = pbuf;

  return pbuf;
}

static char *proxy_ftp_telnet_gets(char *buf, size_t buflen,
    pr_netio_stream_t *nstrm) {
  char *buf_ptr = buf;
  unsigned char cp;
  int nread, saw_newline = FALSE;
  pr_buffer_t *pbuf = NULL;

  if (buflen == 0) {
    errno = EINVAL;
    return NULL;
  }

  buflen--;

  if (nstrm->strm_buf != NULL) {
    pbuf = nstrm->strm_buf;

  } else {
    pbuf = netio_buffer_alloc(nstrm);
  }

  while (buflen > 0) {
    /* Is the buffer empty? */
    if (pbuf->current == NULL ||
        pbuf->remaining == pbuf->buflen) {

      nread = pr_netio_read(nstrm, pbuf->buf,
        (buflen < pbuf->buflen ? buflen : pbuf->buflen), 4);
      if (nread <= 0) {
        if (buf_ptr != buf) {
          *buf_ptr = '\0';
          return buf;
        }

        return NULL;
      }

      pbuf->remaining = pbuf->buflen - nread;
      pbuf->current = pbuf->buf;

      pr_event_generate("mod_proxy.ctrl-read", pbuf);
    }

    nread = pbuf->buflen - pbuf->remaining;

    /* Expensive copying of bytes while we look for the trailing LF. */
    while (buflen > 0 &&
           nread > 0 &&
           *pbuf->current != '\n' &&
           nread--) {
      pr_signals_handle();

      cp = *pbuf->current++;
      pbuf->remaining++;
      *buf_ptr++ = cp;
      buflen--;
    }

    if (buflen > 0 &&
        nread > 0 &&
        *pbuf->current == '\n') {
      buflen--;
      nread--;
      *buf_ptr++ = *pbuf->current++;
      pbuf->remaining++;

      saw_newline = TRUE;
      break;
    }

    if (nread == 0) {
      pbuf->current = NULL;
    }
  }

  if (saw_newline == FALSE) {
    /* If we haven't seen a newline, then assume the server is deliberately
     * sending a too-long response, trying to exploit buffer sizes and make
     * the proxy make some possibly bad assumptions.
     */

    errno = E2BIG;
    return NULL;
  }

  *buf_ptr = '\0';
  return buf;
}

/* XXX Now I need matching proxy_ftp_send_data/proxy_ftp_recv_data functions,
 * for data transfers.
 *
 * To multiplex betwen calling recv_resp and recv_data, would need a select(2)
 * on the fds (ctrl and data) for readability...
 */

static int proxy_ftp_send_cmd(pool *p, conn_t *ctrl_conn, const char *fmt,
    ...) {
  va_list msg;
  int res;

  va_start(msg, fmt);
  res = pr_netio_vprintf(ctrl_conn->outstrm, fmt, msg);
  va_end(msg);

  return res;
}

static pr_response_t *proxy_ftp_recv_resp(pool *p, conn_t *ctrl_conn) {
  char buf[PR_TUNABLE_BUFFER_SIZE];
  pr_response_t *resp = NULL;

  while (TRUE) {
    char c, *ptr;
    int resp_code;
    size_t buflen;

    pr_signals_handle();

    memset(buf, '\0', sizeof(buf));
    if (proxy_ftp_telnet_gets(buf, sizeof(buf)-1, ctrl_conn->instrm) == NULL) {
      return NULL;
    }

    buflen = strlen(buf);

    /* Remove any trailing CRs, LFs. */
    while (buflen > 0 &&
           (buf[buflen-1] == '\r' || buf[buflen-1] == '\n')) {
      pr_signals_handle();

      buf[buflen-1] = '\0';
      buflen--;
    }

    /* If we are the first line of the response, the first three characters
     * MUST be numeric, followed by a space or hypen.  Anything else is
     * nonconformant with RFC 959.
     *
     * If we are NOT the first line of the response, then we are probably
     * handling a multiline response,.   If the first character is a space,
     * then this is a continuation line.  Otherwise, the first three characters
     * MUST be numeric, AND MUST match the numeric code from the first line.
     * This indicates the last line in the multiline response -- and the
     * character after the numerics MUST be a space.
     * 
     */
    if (resp == NULL) {
      /* First line of a possibly multiline response (or just the only line). */
      if (buflen < 4) {
        pr_log_debug(DEBUG2,
          "read %lu characters of response, needed at least %d",
          (unsigned long) buflen, 4);
        errno = EINVAL;
        return NULL;
      }

      if (!isdigit((int) buf[0]) ||
          !isdigit((int) buf[1]) ||
          !isdigit((int) buf[2])) {
        pr_log_debug(DEBUG2,
          "non-numeric characters in start of response data: '%c%c%c'",
          buf[0], buf[1], buf[2]);
        errno = EINVAL;
        return NULL;
      }

      if (buf[3] != ' ' &&
          buf[3] != '-') {
        pr_log_debug(DEBUG2,
          "unexpected character '%c' following numeric response code", buf[3]);
        errno = EINVAL;
        return NULL;
      }

      resp = (pr_response_t *) pcalloc(p, sizeof(pr_response_t));

    } else {
      if (buflen >= 1) {
        if (buf[0] == ' ') {
          /* Continuation line; append it the existing response. */
          if (buflen > 1) {
            resp->msg = pstrcat(p, resp->msg, "\n", &(buf[1]), NULL);
          }
          continue;

        } else {
          /* Possible ending line of multiline response. */
          if (buflen < 4) {
            errno = EINVAL;
            return NULL;
          }

          if (!isdigit((int) buf[0]) ||
              !isdigit((int) buf[1]) ||
              !isdigit((int) buf[2])) {
            pr_log_debug(DEBUG2,
              "non-numeric characters in end of response data: '%c%c%c'",
              buf[0], buf[1], buf[2]);
            errno = EINVAL;
            return NULL;
          }

          if (buf[3] != ' ') {
            errno = EINVAL;
            return NULL;
          }
        }
      }
    }

    ptr = &(buf[3]);
    c = *ptr;
    *ptr = '\0';
    resp_code = atoi(buf);
    if (resp_code < 100 ||
        resp_code >= 700) {
      /* Outside of the expected/defined FTP response code range. */
      pr_log_pri(PR_LOG_NOTICE, "invalid FTP response code %d received",
        resp_code);
      errno = EINVAL;
      return NULL;
    }

    if (resp->num == NULL) {
      resp->num = pstrdup(p, buf);

    } else {
      /* Make sure the last line of the multiline response uses the same
       * response code.
       */
      if (strncmp(resp->num, buf, 3) != 0) {
        pr_log_pri(PR_LOG_NOTICE,
          "invalid multiline FTP response: mismatched starting response "
          "code (%s) and ending response code (%s)", resp->num, buf);
        errno = EINVAL;
        return NULL;
      }
    }

    if (resp->msg == NULL) {
      if (buflen > 4) {
        resp->msg = pstrdup(p, ptr + 1);

      } else {
        resp->msg = "";
      }

      /* If the character after the response code was a space, then this is
       * a single line response; we can be done now.
       */
      if (c == ' ') {
        break;
      }

    } else {
      if (buflen > 4) {
        resp->msg = pstrcat(p, resp->msg, "\n", ptr + 1, NULL);
      }

      break;
    }
  }

  return resp;
}

struct proxy_ftp_client *proxy_ftp_open(pool *p, pr_netaddr_t *remote_addr,
    unsigned int remote_port) {
  struct proxy_ftp_client *ftp = NULL;
  pool *sub_pool;

  if (p == NULL ||
      remote_addr == NULL) {
    errno = EINVAL;
    return NULL;
  }

  sub_pool = make_sub_pool(p);
  pr_pool_tag(sub_pool, "FTP client pool");

  ftp = pcalloc(sub_pool, sizeof(struct proxy_ftp_client));
  ftp->client_pool = sub_pool;
  ftp->protocol = "ftp";
  ftp->remote_addr = remote_addr;
  ftp->remote_port = remote_port;

  return ftp;
}

void proxy_ftp_close(struct proxy_ftp_client **ftp) {
  if (ftp == NULL ||
      *ftp == NULL) {
    return;
  }

  if ((*ftp)->ctrl_conn != NULL) {
    pr_inet_close((*ftp)->client_pool, (*ftp)->ctrl_conn);
    (*ftp)->ctrl_conn = NULL;
  }

  if ((*ftp)->data_conn != NULL) {
    pr_inet_close((*ftp)->client_pool, (*ftp)->data_conn);
    (*ftp)->data_conn = NULL;
  }

  /* XXX How to close the ftp->proxy_client member? */

  destroy_pool((*ftp)->client_pool);
  *ftp = NULL;
}

void pr_signals_handle(void) {
  table_handling_signal(TRUE);

  if (errno == EINTR &&
      PR_TUNABLE_EINTR_RETRY_INTERVAL > 0) {
    struct timeval tv;
    unsigned long interval_usecs = PR_TUNABLE_EINTR_RETRY_INTERVAL * 1000000;

    tv.tv_sec = (interval_usecs / 1000000);
    tv.tv_usec = (interval_usecs - (tv.tv_sec * 1000000));

    pr_timer_usleep(interval_usecs);
  }

  if (recvd_signal_flags == 0) {
    table_handling_signal(FALSE);
    return;
  }

  while (recvd_signal_flags & RECEIVED_SIG_ALRM) {
    if (recvd_signal_flags & RECEIVED_SIG_ALRM) {
      recvd_signal_flags &= ~RECEIVED_SIG_ALRM;
      pr_trace_msg("signal", 9, "handling SIGALRM (signal %d)", SIGALRM);
      handle_alarm();
    }
  }

  table_handling_signal(FALSE);
}

int main(int argc, char *argv[]) {
  pool *p;
  const char *remote_name;
  pr_netaddr_t *remote_addr;
  conn_t *client_conn, *ctrl_conn;
  int remote_port, res, timerno;
  pr_response_t *resp;

  /* Seed the random number generator. */
  /* XXX Use random(3) in the future? */
  srand((unsigned int) (time(NULL) * getpid()));

  init_pools();
  init_privs();
  init_log();
  init_regexp();
  init_inet();
  init_netio();
  init_netaddr();
  init_fs();
  init_class();
  init_config();
  init_stash();

  pr_netaddr_disable_ipv6();

  pr_log_setdebuglevel(10);
  log_stderr(TRUE);
  pr_trace_use_stderr(TRUE);
  pr_trace_set_levels("DEFAULT", 1, 20);

  p = make_sub_pool(permanent_pool);
  pr_pool_tag(p, "FTP Client Pool");

  remote_name = "ftp.proftpd.org";

  remote_addr = pr_netaddr_get_addr(p, remote_name, NULL);
  if (remote_addr == NULL) {
    fprintf(stderr, "Failed to get addr for '%s': %s\n", remote_name,
      strerror(errno));
    destroy_pool(p);
    return 1;
  } 

  remote_port = 21;
 
  fprintf(stdout, "Resolved name '%s' to IP address '%s'\n", remote_name,
    pr_netaddr_get_ipstr(remote_addr));

  timerno = pr_timer_add(5, -1, NULL, connect_timeout_cb,
    "FTP client connect timeout");
  if (timerno <= 0) {
    fprintf(stderr, "Error register connect timer: %s\n", strerror(errno));
    destroy_pool(p);
    return 1;
  }

  /* Connect to the addr */
  client_conn = pr_inet_create_conn(p, -1, NULL, INPORT_ANY, FALSE);
  if (client_conn == NULL) {
    fprintf(stderr, "Error creating connection: %s\n", strerror(errno));

    pr_timer_remove(timerno, NULL);
    destroy_pool(p);
    return 1;
  }

  /* XXX And now I have an easy way to reproduce Bug#3802! */

  res = pr_inet_connect_nowait(p, client_conn, remote_addr, remote_port);
  if (res < 0) {
    fprintf(stderr, "Error starting connect to %s:%d: %s\n", remote_name,
      remote_port, strerror(errno));

    pr_timer_remove(timerno, NULL);
    pr_inet_close(p, client_conn);
    destroy_pool(p);
    return 1;
  }

  /* XXX Need to test what happens when connect to same machine */

  if (res == 0) {
    /* Not yet connected. */

    connect_timeout_strm = pr_netio_open(p, PR_NETIO_STRM_OTHR,
      client_conn->listen_fd, PR_NETIO_IO_RD);
    if (connect_timeout_strm == NULL) {
      fprintf(stderr, "Error opening stream to %s:%d: %s\n", remote_name,
        remote_port, strerror(errno));

      pr_timer_remove(timerno, NULL);
      pr_inet_close(p, client_conn);
      destroy_pool(p);
      return 1;
    }

    pr_netio_set_poll_interval(connect_timeout_strm, 1);

    switch (pr_netio_poll(connect_timeout_strm)) {
      case 1: {
        /* Aborted, timed out */
        if (connect_timeout_reached) {
          errno = ETIMEDOUT;

          fprintf(stderr, "Connecting to %s:%d timed out after %d secs: %s\n",
            remote_name, remote_port, 5, strerror(errno));
          pr_netio_close(connect_timeout_strm);
          connect_timeout_strm = NULL;

          pr_timer_remove(timerno, NULL);
          pr_inet_close(p, client_conn);
          destroy_pool(p);
          return 1;
        }

        break;
      }

      case -1: {
        /* Error */
        int xerrno = errno;

        fprintf(stderr, "Error connecting to %s:%d: %s\n", remote_name,
          remote_port, strerror(xerrno));
        pr_netio_close(connect_timeout_strm);
        connect_timeout_strm = NULL;

        pr_timer_remove(timerno, NULL);
        pr_inet_close(p, client_conn);
        return 1;
      }

      default: {
        /* Connected */
        client_conn->mode = CM_OPEN;
        pr_timer_remove(timerno, NULL);

        if (pr_inet_get_conn_info(client_conn, client_conn->listen_fd) < 0) {
          fprintf(stderr, "Error obtaining local socket info on fd %d: %s\n",
            client_conn->listen_fd, strerror(errno));

          pr_inet_close(p, client_conn);
          destroy_pool(p);
          return 1;
        }

        break;
      }
    }
  }

  fprintf(stdout, "Successfully connected to %s:%d from %s:%d\n", remote_name,
    remote_port, pr_netaddr_get_ipstr(client_conn->local_addr),
    ntohs(pr_netaddr_get_port(client_conn->local_addr)));

  ctrl_conn = pr_inet_openrw(p, client_conn, NULL, PR_NETIO_STRM_CTRL,
    -1, -1, -1, FALSE);
  if (ctrl_conn == NULL) {
    fprintf(stderr, "Error opening control connection: %s\n", strerror(errno));

    pr_inet_close(p, client_conn);
    destroy_pool(p);
    return 1;
  }

  fprintf(stdout, "Reading response from %s:%d\n", remote_name, remote_port);

  /* We have our own version of netio_telnet_gets(), with the buffering to
   * handle reassembly of a full FTP response out of multiple TCP packets,
   * and without the handling of Telnet codes.
   */

  /* Read the initial banner/response. */
  resp = proxy_ftp_recv_resp(p, ctrl_conn);
  if (resp == NULL) {
    fprintf(stderr, "Error getting response from server: %s\n",
      strerror(errno));

  } else {
    fprintf(stdout, "Response: \"%s\" (%s)\n", resp->msg, resp->num);
  }

  /* SYST */
  fprintf(stdout, "Command: \"%s\"\n", C_SYST);
  res = proxy_ftp_send_cmd(p, ctrl_conn, "%s\r\n", C_SYST);
  if (res < 0) {
    fprintf(stderr, "Error sending command to server: %s", strerror(errno));
  }

  resp = proxy_ftp_recv_resp(p, ctrl_conn);
  if (resp == NULL) {
    fprintf(stderr, "Error getting response from server: %s\n",
      strerror(errno));

  } else {
    fprintf(stdout, "Response: \"%s\" (%s)\n", resp->msg, resp->num);
  }

  /* PWD */
  fprintf(stdout, "Command: \"%s\"\n", C_PWD);
  res = proxy_ftp_send_cmd(p, ctrl_conn, "%s\r\n", C_PWD);
  if (res < 0) {
    fprintf(stderr, "Error sending command to server: %s", strerror(errno));
  }

  resp = proxy_ftp_recv_resp(p, ctrl_conn);
  if (resp == NULL) {
    fprintf(stderr, "Error getting response from server: %s\n",
      strerror(errno));

  } else {
    fprintf(stdout, "Response: \"%s\" (%s)\n", resp->msg, resp->num);
  }

  /* FEAT */
  fprintf(stdout, "Command: \"%s\"\n", C_FEAT);
  res = proxy_ftp_send_cmd(p, ctrl_conn, "%s\r\n", C_FEAT);
  if (res < 0) {
    fprintf(stderr, "Error sending command to server: %s", strerror(errno));
  }

  resp = proxy_ftp_recv_resp(p, ctrl_conn);
  if (resp == NULL) {
    fprintf(stderr, "Error getting response from server: %s\n",
      strerror(errno));

  } else {
    fprintf(stdout, "Response: \"%s\" (%s)\n", resp->msg, resp->num);
  }

  /* LANG */
  fprintf(stdout, "Command: \"%s\"\n", C_LANG);
  res = proxy_ftp_send_cmd(p, ctrl_conn, "%s\r\n", C_LANG);
  if (res < 0) {
    fprintf(stderr, "Error sending command to server: %s", strerror(errno));
  }

  resp = proxy_ftp_recv_resp(p, ctrl_conn);
  if (resp == NULL) {
    fprintf(stderr, "Error getting response from server: %s\n",
      strerror(errno));

  } else {
    fprintf(stdout, "Response: \"%s\" (%s)\n", resp->msg, resp->num);
  }

  /* USER */
  fprintf(stdout, "Command: \"%s\"\n", C_USER);
  res = proxy_ftp_send_cmd(p, ctrl_conn, "%s %s\r\n", C_USER, "ftp");
  if (res < 0) {
    fprintf(stderr, "Error sending command to server: %s", strerror(errno));
  }

  resp = proxy_ftp_recv_resp(p, ctrl_conn);
  if (resp == NULL) {
    fprintf(stderr, "Error getting response from server: %s\n",
      strerror(errno));

  } else {
    fprintf(stdout, "Response: \"%s\" (%s)\n", resp->msg, resp->num);
  }

  /* PASS */
  fprintf(stdout, "Command: \"%s\"\n", C_PASS);
  res = proxy_ftp_send_cmd(p, ctrl_conn, "%s %s\r\n", C_PASS, "ftp@nospam.org");
  if (res < 0) {
    fprintf(stderr, "Error sending command to server: %s", strerror(errno));
  } 
  
  resp = proxy_ftp_recv_resp(p, ctrl_conn);
  if (resp == NULL) {
    fprintf(stderr, "Error getting response from server: %s\n",
      strerror(errno));

  } else {
    fprintf(stdout, "Response: \"%s\" (%s)\n", resp->msg, resp->num);
  }

  /* Disconnect */
  fprintf(stdout, "Command: \"%s\"\n", C_QUIT);
  res = proxy_ftp_send_cmd(p, ctrl_conn, "%s\r\n", C_QUIT);
  if (res < 0) {
    fprintf(stderr, "Error sending command to server: %s", strerror(errno));
  }

  resp = proxy_ftp_recv_resp(p, ctrl_conn);
  if (resp == NULL) {
    fprintf(stderr, "Error getting response from server: %s\n",
      strerror(errno));

  } else {
    fprintf(stdout, "Response: \"%s\" (%s)\n", resp->msg, resp->num);
  }

  pr_inet_close(p, ctrl_conn);
  pr_inet_close(p, client_conn);
  destroy_pool(p);
  return 0;
}
