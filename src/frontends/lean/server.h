/*
Copyright (c) 2014 Microsoft Corporation. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.

Author: Leonardo de Moura
*/
#pragma once
#include <vector>
#include <string>
#include <unordered_map>
#include "util/interrupt.h"
#include "library/definition_cache.h"
#include "frontends/lean/parser.h"
#include "frontends/lean/info_manager.h"

namespace lean {
/**
   \brief Class for managing an input stream used to communicate with
   external processes.
*/
class server {
    class worker;
    class file {
        friend class server::worker;
        std::string               m_fname;
        mutable mutex             m_lines_mutex;
        std::vector<std::string>  m_lines;
        snapshot_vector           m_snapshots;
        info_manager              m_info;

        unsigned find(unsigned linenum);
        unsigned copy_to(std::string & block, unsigned starting_from);
    public:
        file(std::istream & in, std::string const & fname);
        void replace_line(unsigned linenum, std::string const & new_line);
        void insert_line(unsigned linenum, std::string const & new_line);
        void remove_line(unsigned linenum);
        info_manager const & infom() const { return m_info; }
    };
    typedef std::shared_ptr<file>                     file_ptr;
    typedef std::unordered_map<std::string, file_ptr> file_map;
    class worker {
        snapshot             m_empty_snapshot;
        definition_cache &   m_cache;
        atomic_bool          m_busy;
        file_ptr             m_todo_file;
        unsigned             m_todo_linenum;
        options              m_todo_options;
        mutex                m_todo_mutex;
        condition_variable   m_todo_cv;
        file_ptr             m_last_file;
        atomic_bool          m_terminate;
        interruptible_thread m_thread;
    public:
        worker(environment const & env, io_state const & ios, definition_cache & cache);
        ~worker();
        void set_todo(file_ptr const & f, unsigned linenum, options const & o);
        void request_interrupt();
        void wait();
    };

    file_map                  m_file_map;
    file_ptr                  m_file;
    environment               m_env;
    io_state                  m_ios;
    std::ostream &            m_out;
    unsigned                  m_num_threads;
    snapshot                  m_empty_snapshot;
    definition_cache          m_cache;
    worker                    m_worker;

    void load_file(std::string const & fname);
    void visit_file(std::string const & fname);
    void check_file();
    void replace_line(unsigned linenum, std::string const & new_line);
    void insert_line(unsigned linenum, std::string const & new_line);
    void remove_line(unsigned linenum);
    void show_info(unsigned linenum);
    void process_from(unsigned linenum);
    void set_option(std::string const & line);
    void eval_core(environment const & env, options const & o, std::string const & line);
    void eval(std::string const & line);
    unsigned find(unsigned linenum);
    void read_line(std::istream & in, std::string & line);
    void interrupt_worker();
    unsigned get_linenum(std::string const & line, std::string const & cmd);

public:
    server(environment const & env, io_state const & ios, unsigned num_threads = 1);
    ~server();
    bool operator()(std::istream & in);
};
}
