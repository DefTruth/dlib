// Copyright (C) 2012  Davis E. King (davis@dlib.net)
// License: Boost Software License   See LICENSE.txt for the full license.
#ifndef DLIB_BsP_H__
#define DLIB_BsP_H__

#include "bsp_abstract.h"
#include "../sockets.h"
#include "../array.h"
#include "../smart_pointers.h"
#include "../sockstreambuf.h"
#include "../string.h"
#include "../serialize.h"
#include "../map.h"
#include "../ref.h"
#include <deque>
#include <vector>

namespace dlib
{

// ----------------------------------------------------------------------------------------

    namespace impl1
    {
        inline void null_notify(
            unsigned short
        ) {}

        struct bsp_con
        {
            bsp_con(
                const std::pair<std::string,unsigned short>& dest
            ) : 
                con(connect(dest.first,dest.second)),
                buf(con),
                stream(&buf),
                terminated(false)
            {}

            bsp_con(
               scoped_ptr<connection>& conptr 
            ) : 
                buf(conptr),
                stream(&buf),
                terminated(false)
            {
                // make sure we own the connection
                conptr.swap(con);
            }

            scoped_ptr<connection> con;
            sockstreambuf::kernel_2a buf;
            std::iostream stream;
            bool terminated;
        };

        typedef dlib::map<unsigned long, scoped_ptr<bsp_con> >::kernel_1a_c map_id_to_con;

        void connect_all (
            map_id_to_con& cons,
            const std::vector<std::pair<std::string,unsigned short> >& hosts,
            unsigned long node_id
        );
        /*!
            ensures
                - creates connections to all the given hosts and stores them into cons
        !*/

        void send_out_connection_orders (
            map_id_to_con& cons,
            const std::vector<std::pair<std::string,unsigned short> >& hosts
        );

    // ------------------------------------------------------------------------------------

        struct hostinfo
        {
            hostinfo() {}
            hostinfo (
                const std::string& ip_,
                unsigned short port_,
                unsigned long node_id_
            ) : 
                ip(ip_),
                port(port_),
                node_id(node_id_)
            {
            }

            std::string ip;
            unsigned short port;
            unsigned long node_id;
        };

        inline void serialize (
            const hostinfo& item,
            std::ostream& out
        )
        {
            dlib::serialize(item.ip, out);
            dlib::serialize(item.port, out);
            dlib::serialize(item.node_id, out);
        }

        inline void deserialize (
            hostinfo& item,
            std::istream& in
        )
        {
            dlib::deserialize(item.ip, in);
            dlib::deserialize(item.port, in);
            dlib::deserialize(item.node_id, in);
        }

    // ------------------------------------------------------------------------------------

        void connect_all_hostinfo (
            map_id_to_con& cons,
            const std::vector<hostinfo>& hosts,
            unsigned long node_id,
            std::string& error_string 
        );

    // ------------------------------------------------------------------------------------

        template <
            typename port_notify_function_type
        >
        void listen_and_connect_all(
            unsigned long& node_id,
            map_id_to_con& cons,
            unsigned short port,
            port_notify_function_type port_notify_function
        )
        {
            cons.clear();
            scoped_ptr<listener> list;
            const int status = create_listener(list, port);
            if (status == PORTINUSE)
            {
                throw socket_error("Unable to create listening port " + cast_to_string(port) +
                                   ".  The port is already in use");
            }
            else if (status != 0)
            {
                throw socket_error("Unable to create listening port " + cast_to_string(port) );
            }

            port_notify_function(list->get_listening_port());

            scoped_ptr<connection> con;
            if (list->accept(con))
            {
                throw socket_error("Error occurred while accepting new connection");
            }

            scoped_ptr<bsp_con> temp(new bsp_con(con));

            unsigned long remote_node_id;
            dlib::deserialize(remote_node_id, temp->stream);
            dlib::deserialize(node_id, temp->stream);
            std::vector<hostinfo> targets; 
            dlib::deserialize(targets, temp->stream);
            unsigned long num_incoming_connections;
            dlib::deserialize(num_incoming_connections, temp->stream);

            cons.add(remote_node_id,temp);

            // make a thread that will connect to all the targets
            map_id_to_con cons2;
            std::string error_string;
            thread_function thread(connect_all_hostinfo, dlib::ref(cons2), dlib::ref(targets), node_id, dlib::ref(error_string));
            if (error_string.size() != 0)
                throw socket_error(error_string);

            // accept any incoming connections
            for (unsigned long i = 0; i < num_incoming_connections; ++i)
            {
                // If it takes more than 10 seconds for the other nodes to connect to us
                // then something has gone horribly wrong and it almost certainly will
                // never connect at all.  So just give up if that happens.
                const unsigned long timeout_milliseconds = 10000;
                if (list->accept(con, timeout_milliseconds))
                {
                    throw socket_error("Error occurred while accepting new connection");
                }

                temp.reset(new bsp_con(con));

                dlib::deserialize(remote_node_id, temp->stream);
                cons.add(remote_node_id,temp);
            }


            // put all the connections created by the thread into cons
            thread.wait();
            while (cons2.size() > 0)
            {
                unsigned long id;
                scoped_ptr<bsp_con> temp;
                cons2.remove_any(id,temp);
                cons.add(id,temp);
            }
        }

    // ------------------------------------------------------------------------------------

        struct msg_data
        {
            shared_ptr<std::string> data;
            unsigned long sender_id;
            char msg_type;
        };


        class thread_safe_deque
        {
        public:
            thread_safe_deque() : sig(class_mutex),disabled(false) {}

            ~thread_safe_deque()
            {
                disable();
            }

            void disable()
            {
                auto_mutex lock(class_mutex);
                disabled = true;
                sig.broadcast();
            }

            unsigned long size() const { return data.size(); }

            void push_front( const msg_data& item)
            {
                auto_mutex lock(class_mutex);
                data.push_front(item);
                sig.signal();
            }

            void push_and_consume( msg_data& item)
            {
                auto_mutex lock(class_mutex);
                data.push_back(item);
                // do this here so that we don't have to worry about different threads touching the shared_ptr.
                item.data.reset(); 
                sig.signal();
            }

            bool pop ( 
                msg_data& item
            )
            /*!
                ensures
                    - if (this function returns true) then
                        - #item == the next thing from the queue
                    - else
                        - this object is disabled
            !*/
            {
                auto_mutex lock(class_mutex);
                while (data.size() == 0 && !disabled)
                    sig.wait();

                if (disabled)
                    return false;

                item = data.front();
                data.pop_front();

                return true;
            }

        private:
            std::deque<msg_data> data;
            dlib::mutex class_mutex;
            dlib::signaler sig;
            bool disabled;
        };


    }

// ----------------------------------------------------------------------------------------

    class bsp_context : noncopyable
    {

    public:

        template <typename T>
        void send(
            const T& item,
            unsigned long target_node_id
        ) 
        {
            std::ostringstream sout;
            serialize(item, sout);
            send_data(sout.str(), target_node_id);
        }

        template <typename T>
        void broadcast (
            const T& item
        ) 
        {
            std::ostringstream sout;
            serialize(item, sout);
            for (unsigned long i = 0; i < number_of_nodes(); ++i)
            {
                // Don't send to yourself.
                if (i == node_id())
                    continue;

                send_data(sout.str(), i);
            }
        }

        unsigned long node_id (
        ) const { return _node_id; }

        unsigned long number_of_nodes (
        ) const { return _cons.size()+1; }

        void receive (
        )
        {
            unsigned long id;
            shared_ptr<std::string> temp;
            if (receive_data(temp,id))
                throw dlib::socket_error("Call to bsp_context::receive() got an unexpected message.");
        }

        template <typename T>
        bool receive (
            T& item
        ) 
        {
            unsigned long sending_node_id;
            return receive(item, sending_node_id);
        }

        template <typename T>
        bool receive (
            T& item,
            unsigned long& sending_node_id
        ) 
        {
            shared_ptr<std::string> temp;
            if (receive_data(temp, sending_node_id))
            {
                std::istringstream sin(*temp);
                deserialize(item, sin);
                return true;
            }
            else
            {
                return false;
            }
        }

        ~bsp_context();

    private:

        bsp_context();

        bsp_context(
            unsigned long node_id_,
            impl1::map_id_to_con& cons_
        );

        void close_all_connections_gracefully();
        /*!
            ensures
                - closes all the connections to other nodes and lets them know that
                  we are terminating normally rather than as the result of some kind
                  of error.
        !*/

        bool receive_data (
            shared_ptr<std::string>& item,
            unsigned long& sending_node_id
        );


        void send_byte (
            char val,
            unsigned long target_node_id
        );

        void broadcast_byte (
            char val
        );

        void send_data(
            const std::string& item,
            unsigned long target_node_id
        );
        /*!
            requires
                - target_node_id < number_of_nodes()
                - target_node_id != node_id()
            ensures
                - sends a copy of item to the node with the given id.
        !*/




        unsigned long outstanding_messages;
        unsigned long num_waiting_nodes;
        unsigned long num_terminated_nodes;

        impl1::thread_safe_deque msg_buffer;

        impl1::map_id_to_con& _cons;
        const unsigned long _node_id;
        array<scoped_ptr<thread_function> > threads;

    // -----------------------------------

        template <
            typename funct_type
            >
        friend void bsp_connect (
            const std::vector<std::pair<std::string,unsigned short> >& hosts,
            funct_type funct
        );

        template <
            typename funct_type,
            typename ARG1
            >
        friend void bsp_connect (
            const std::vector<std::pair<std::string,unsigned short> >& hosts,
            funct_type funct,
            ARG1 arg1
        );

        template <
            typename funct_type,
            typename ARG1,
            typename ARG2
            >
        friend void bsp_connect (
            const std::vector<std::pair<std::string,unsigned short> >& hosts,
            funct_type funct,
            ARG1 arg1,
            ARG2 arg2
        );

        template <
            typename funct_type,
            typename ARG1,
            typename ARG2,
            typename ARG3
            >
        friend void bsp_connect (
            const std::vector<std::pair<std::string,unsigned short> >& hosts,
            funct_type funct,
            ARG1 arg1,
            ARG2 arg2,
            ARG3 arg3
        );

        template <
            typename funct_type,
            typename ARG1,
            typename ARG2,
            typename ARG3,
            typename ARG4
            >
        friend void bsp_connect (
            const std::vector<std::pair<std::string,unsigned short> >& hosts,
            funct_type funct,
            ARG1 arg1,
            ARG2 arg2,
            ARG3 arg3,
            ARG4 arg4
        );

    // -----------------------------------

        template <
            typename port_notify_function_type,
            typename funct_type
            >
        friend void bsp_listen_dynamic_port (
            unsigned short listening_port,
            port_notify_function_type port_notify_function,
            funct_type funct
        );

        template <
            typename port_notify_function_type,
            typename funct_type,
            typename ARG1
            >
        friend void bsp_listen_dynamic_port (
            unsigned short listening_port,
            port_notify_function_type port_notify_function,
            funct_type funct,
            ARG1 arg1
        );

        template <
            typename port_notify_function_type,
            typename funct_type,
            typename ARG1,
            typename ARG2
            >
        friend void bsp_listen_dynamic_port (
            unsigned short listening_port,
            port_notify_function_type port_notify_function,
            funct_type funct,
            ARG1 arg1,
            ARG2 arg2
        );

        template <
            typename port_notify_function_type,
            typename funct_type,
            typename ARG1,
            typename ARG2,
            typename ARG3
            >
        friend void bsp_listen_dynamic_port (
            unsigned short listening_port,
            port_notify_function_type port_notify_function,
            funct_type funct,
            ARG1 arg1,
            ARG2 arg2,
            ARG3 arg3
        );

        template <
            typename port_notify_function_type,
            typename funct_type,
            typename ARG1,
            typename ARG2,
            typename ARG3,
            typename ARG4
            >
        friend void bsp_listen_dynamic_port (
            unsigned short listening_port,
            port_notify_function_type port_notify_function,
            funct_type funct,
            ARG1 arg1,
            ARG2 arg2,
            ARG3 arg3,
            ARG4 arg4
        );

    // -----------------------------------

    };

// ----------------------------------------------------------------------------------------
// ----------------------------------------------------------------------------------------
// ----------------------------------------------------------------------------------------

    template <
        typename funct_type
        >
    void bsp_connect (
        const std::vector<std::pair<std::string,unsigned short> >& hosts,
        funct_type funct
    )
    {
        impl1::map_id_to_con cons;
        const unsigned long node_id = 0;
        connect_all(cons, hosts, node_id);
        send_out_connection_orders(cons, hosts);
        bsp_context obj(node_id, cons);
        funct(obj);
        obj.close_all_connections_gracefully();
    }

// ----------------------------------------------------------------------------------------

    template <
        typename funct_type,
        typename ARG1
        >
    void bsp_connect (
        const std::vector<std::pair<std::string,unsigned short> >& hosts,
        funct_type funct,
        ARG1 arg1
    )
    {
        impl1::map_id_to_con cons;
        const unsigned long node_id = 0;
        connect_all(cons, hosts, node_id);
        send_out_connection_orders(cons, hosts);
        bsp_context obj(node_id, cons);
        funct(obj,arg1);
        obj.close_all_connections_gracefully();
    }

// ----------------------------------------------------------------------------------------

    template <
        typename funct_type,
        typename ARG1,
        typename ARG2
        >
    void bsp_connect (
        const std::vector<std::pair<std::string,unsigned short> >& hosts,
        funct_type funct,
        ARG1 arg1,
        ARG2 arg2
    )
    {
        impl1::map_id_to_con cons;
        const unsigned long node_id = 0;
        connect_all(cons, hosts, node_id);
        send_out_connection_orders(cons, hosts);
        bsp_context obj(node_id, cons);
        funct(obj,arg1,arg2);
        obj.close_all_connections_gracefully();
    }

// ----------------------------------------------------------------------------------------

    template <
        typename funct_type,
        typename ARG1,
        typename ARG2,
        typename ARG3
        >
    void bsp_connect (
        const std::vector<std::pair<std::string,unsigned short> >& hosts,
        funct_type funct,
        ARG1 arg1,
        ARG2 arg2,
        ARG3 arg3
    )
    {
        impl1::map_id_to_con cons;
        const unsigned long node_id = 0;
        connect_all(cons, hosts, node_id);
        send_out_connection_orders(cons, hosts);
        bsp_context obj(node_id, cons);
        funct(obj,arg1,arg2,arg3);
        obj.close_all_connections_gracefully();
    }

// ----------------------------------------------------------------------------------------

    template <
        typename funct_type,
        typename ARG1,
        typename ARG2,
        typename ARG3,
        typename ARG4
        >
    void bsp_connect (
        const std::vector<std::pair<std::string,unsigned short> >& hosts,
        funct_type funct,
        ARG1 arg1,
        ARG2 arg2,
        ARG3 arg3,
        ARG4 arg4
    )
    {
        impl1::map_id_to_con cons;
        const unsigned long node_id = 0;
        connect_all(cons, hosts, node_id);
        send_out_connection_orders(cons, hosts);
        bsp_context obj(node_id, cons);
        funct(obj,arg1,arg2,arg3,arg4);
        obj.close_all_connections_gracefully();
    }

// ----------------------------------------------------------------------------------------
// ----------------------------------------------------------------------------------------
// ----------------------------------------------------------------------------------------

    template <
        typename funct_type
        >
    void bsp_listen (
        unsigned short listening_port,
        funct_type funct
    )
    {
        bsp_listen_dynamic_port(listening_port, impl1::null_notify, funct);
    }

// ----------------------------------------------------------------------------------------

    template <
        typename funct_type,
        typename ARG1
        >
    void bsp_listen (
        unsigned short listening_port,
        funct_type funct,
        ARG1 arg1
    )
    {
        bsp_listen_dynamic_port(listening_port, impl1::null_notify, funct, arg1);
    }

// ----------------------------------------------------------------------------------------

    template <
        typename funct_type,
        typename ARG1,
        typename ARG2
        >
    void bsp_listen (
        unsigned short listening_port,
        funct_type funct,
        ARG1 arg1,
        ARG2 arg2
    )
    {
        bsp_listen_dynamic_port(listening_port, impl1::null_notify, funct, arg1, arg2);
    }

// ----------------------------------------------------------------------------------------

    template <
        typename funct_type,
        typename ARG1,
        typename ARG2,
        typename ARG3
        >
    void bsp_listen (
        unsigned short listening_port,
        funct_type funct,
        ARG1 arg1,
        ARG2 arg2,
        ARG3 arg3
    )
    {
        bsp_listen_dynamic_port(listening_port, impl1::null_notify, funct, arg1, arg2, arg3);
    }

// ----------------------------------------------------------------------------------------

    template <
        typename funct_type,
        typename ARG1,
        typename ARG2,
        typename ARG3,
        typename ARG4
        >
    void bsp_listen (
        unsigned short listening_port,
        funct_type funct,
        ARG1 arg1,
        ARG2 arg2,
        ARG3 arg3,
        ARG4 arg4
    )
    {
        bsp_listen_dynamic_port(listening_port, impl1::null_notify, funct, arg1, arg2, arg3, arg4);
    }

// ----------------------------------------------------------------------------------------
// ----------------------------------------------------------------------------------------

    template <
        typename port_notify_function_type,
        typename funct_type
        >
    void bsp_listen_dynamic_port (
        unsigned short listening_port,
        port_notify_function_type port_notify_function,
        funct_type funct
    )
    {
        impl1::map_id_to_con cons;
        unsigned long node_id;
        listen_and_connect_all(node_id, cons, listening_port, port_notify_function);
        bsp_context obj(node_id, cons);
        funct(obj);
        obj.close_all_connections_gracefully();
    }

// ----------------------------------------------------------------------------------------

    template <
        typename port_notify_function_type,
        typename funct_type,
        typename ARG1
        >
    void bsp_listen_dynamic_port (
        unsigned short listening_port,
        port_notify_function_type port_notify_function,
        funct_type funct,
        ARG1 arg1
    )
    {
        impl1::map_id_to_con cons;
        unsigned long node_id;
        listen_and_connect_all(node_id, cons, listening_port, port_notify_function);
        bsp_context obj(node_id, cons);
        funct(obj,arg1);
        obj.close_all_connections_gracefully();
    }

// ----------------------------------------------------------------------------------------

    template <
        typename port_notify_function_type,
        typename funct_type,
        typename ARG1,
        typename ARG2
        >
    void bsp_listen_dynamic_port (
        unsigned short listening_port,
        port_notify_function_type port_notify_function,
        funct_type funct,
        ARG1 arg1,
        ARG2 arg2
    )
    {
        impl1::map_id_to_con cons;
        unsigned long node_id;
        listen_and_connect_all(node_id, cons, listening_port, port_notify_function);
        bsp_context obj(node_id, cons);
        funct(obj,arg1,arg2);
        obj.close_all_connections_gracefully();
    }

// ----------------------------------------------------------------------------------------

    template <
        typename port_notify_function_type,
        typename funct_type,
        typename ARG1,
        typename ARG2,
        typename ARG3
        >
    void bsp_listen_dynamic_port (
        unsigned short listening_port,
        port_notify_function_type port_notify_function,
        funct_type funct,
        ARG1 arg1,
        ARG2 arg2,
        ARG3 arg3
    )
    {
        impl1::map_id_to_con cons;
        unsigned long node_id;
        listen_and_connect_all(node_id, cons, listening_port, port_notify_function);
        bsp_context obj(node_id, cons);
        funct(obj,arg1,arg2,arg3);
        obj.close_all_connections_gracefully();
    }

// ----------------------------------------------------------------------------------------

    template <
        typename port_notify_function_type,
        typename funct_type,
        typename ARG1,
        typename ARG2,
        typename ARG3,
        typename ARG4
        >
    void bsp_listen_dynamic_port (
        unsigned short listening_port,
        port_notify_function_type port_notify_function,
        funct_type funct,
        ARG1 arg1,
        ARG2 arg2,
        ARG3 arg3,
        ARG4 arg4
    )
    {
        impl1::map_id_to_con cons;
        unsigned long node_id;
        listen_and_connect_all(node_id, cons, listening_port, port_notify_function);
        bsp_context obj(node_id, cons);
        funct(obj,arg1,arg2,arg3,arg4);
        obj.close_all_connections_gracefully();
    }
// ----------------------------------------------------------------------------------------
// ----------------------------------------------------------------------------------------
// ----------------------------------------------------------------------------------------

}

#ifdef NO_MAKEFILE
#include "bsp.cpp"
#endif

#endif // DLIB_BsP_H__
