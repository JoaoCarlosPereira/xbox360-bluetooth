#ifndef BT_TRACE_H
#define BT_TRACE_H

/* Uma linha em cada disco que abrir. O console não tem depurador;
 * o arquivo fica na raiz do HD interno e do Mass0. */
void bt_trace(const char *format, ...) __attribute__((format(printf, 1, 2)));

#endif
