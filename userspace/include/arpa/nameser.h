#pragma once

#define NS_PACKETSZ  512
#define NS_MAXDNAME  1025
#define NS_HFIXEDSZ  12
#define NS_QFIXEDSZ  4
#define PACKETSZ     NS_PACKETSZ
#define MAXDNAME     NS_MAXDNAME
#define HFIXEDSZ     NS_HFIXEDSZ
#define QFIXEDSZ     NS_QFIXEDSZ

#define ns_c_in      1
#define ns_t_a       1
#define ns_t_ns      2
#define ns_t_cname   5
#define ns_t_soa     6
#define ns_t_ptr     12
#define ns_t_mx      15
#define ns_t_txt     16
#define ns_t_aaaa    28
#define ns_t_srv     33
#define ns_t_any     255
#define C_IN         ns_c_in
#define T_A          ns_t_a
#define T_NS         ns_t_ns
#define T_CNAME      ns_t_cname
#define T_SOA        ns_t_soa
#define T_PTR        ns_t_ptr
#define T_MX         ns_t_mx
#define T_TXT        ns_t_txt
#define T_AAAA       ns_t_aaaa
#define T_SRV        ns_t_srv
#define T_ANY        ns_t_any
#define QUERY        0
#define ns_o_query   0
