//===-- SectionLoadList.cpp -------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "lldb/Target/SectionLoadList.h"

// C Includes
// C++ Includes
// Other libraries and framework includes
// Project includes
#include "lldb/Core/Log.h"
#include "lldb/Core/Module.h"
#include "lldb/Core/Section.h"
#include "lldb/Core/Stream.h"
#include "lldb/Symbol/Block.h"
#include "lldb/Symbol/Symbol.h"
#include "lldb/Symbol/SymbolContext.h"

using namespace lldb;
using namespace lldb_private;


SectionLoadList::SectionLoadList (const SectionLoadList& rhs) :
    m_addr_to_sect(),
    m_sect_to_addr(),
    m_mutex (Mutex::eMutexTypeRecursive)
{
    Mutex::Locker locker(rhs.m_mutex);
    m_addr_to_sect = rhs.m_addr_to_sect;
    m_sect_to_addr = rhs.m_sect_to_addr;
}

void
SectionLoadList::operator=(const SectionLoadList &rhs)
{
    Mutex::Locker lhs_locker (m_mutex);
    Mutex::Locker rhs_locker (rhs.m_mutex);
    m_addr_to_sect = rhs.m_addr_to_sect;
    m_sect_to_addr = rhs.m_sect_to_addr;
}

bool
SectionLoadList::IsEmpty() const
{
    Mutex::Locker locker(m_mutex);
    return m_addr_to_sect.empty();
}

void
SectionLoadList::Clear ()
{
    Mutex::Locker locker(m_mutex);
    m_addr_to_sect.clear();
    m_sect_to_addr.clear();
}

addr_t
SectionLoadList::GetSectionLoadAddress (const lldb::SectionSP &section) const
{
    // TODO: add support for the same section having multiple load addresses
    addr_t section_load_addr = LLDB_INVALID_ADDRESS;
    if (section)
    {
        Mutex::Locker locker(m_mutex);
        sect_to_addr_collection::const_iterator pos = m_sect_to_addr.find (section.get());
        
        if (pos != m_sect_to_addr.end())
            section_load_addr = pos->second;
    }
    return section_load_addr;
}

bool
SectionLoadList::SetSectionLoadAddress (const lldb::SectionSP &section, addr_t load_addr, bool warn_multiple)
{
    Log *log(lldb_private::GetLogIfAnyCategoriesSet (LIBLLDB_LOG_DYNAMIC_LOADER | LIBLLDB_LOG_VERBOSE));

    ModuleSP module_sp (section->GetModule());
    
    if (module_sp)
    {
        if (log)
        {
            const FileSpec &module_file_spec (module_sp->GetFileSpec());
            log->Printf ("SectionLoadList::%s (section = %p (%s.%s), load_addr = 0x%16.16" PRIx64 ") module = %p",
                         __FUNCTION__,
                         section.get(),
                         module_file_spec.GetPath().c_str(),
                         section->GetName().AsCString(),
                         load_addr,
                         module_sp.get());
        }

        if (section->GetByteSize() == 0)
            return false; // No change

        // Fill in the section -> load_addr map
        Mutex::Locker locker(m_mutex);
        sect_to_addr_collection::iterator sta_pos = m_sect_to_addr.find(section.get());
        if (sta_pos != m_sect_to_addr.end())
        {
            if (load_addr == sta_pos->second)
                return false; // No change...
            else
                sta_pos->second = load_addr;
        }
        else
            m_sect_to_addr[section.get()] = load_addr;

        // Fill in the load_addr -> section map
        addr_to_sect_collection::iterator ats_pos = m_addr_to_sect.find(load_addr);
        if (ats_pos != m_addr_to_sect.end())
        {
            // Some sections are ok to overlap, and for others we should warn. When
            // we have multiple load addresses that correspond to a section, we will
            // allways attribute the section to the be last section that claims it
            // exists at that address. Sometimes it is ok for more that one section
            // to be loaded at a specific load address, and other times it isn't.
            // The "warn_multiple" parameter tells us if we should warn in this case
            // or not. The DynamicLoader plug-in subclasses should know which
            // sections should warn and which shouldn't (darwin shared cache modules
            // all shared the same "__LINKEDIT" sections, so the dynamic loader can
            // pass false for "warn_multiple").
            if (warn_multiple && section != ats_pos->second)
            {
                ModuleSP module_sp (section->GetModule());
                if (module_sp)
                {
                    ModuleSP curr_module_sp (ats_pos->second->GetModule());
                    if (curr_module_sp)
                    {
                        module_sp->ReportWarning ("address 0x%16.16" PRIx64 " maps to more than one section: %s.%s and %s.%s",
                                                  load_addr, 
                                                  module_sp->GetFileSpec().GetFilename().GetCString(), 
                                                  section->GetName().GetCString(),
                                                  curr_module_sp->GetFileSpec().GetFilename().GetCString(),
                                                  ats_pos->second->GetName().GetCString());
                    }
                }
            }
            ats_pos->second = section;
        }
        else
            m_addr_to_sect[load_addr] = section;
        return true;    // Changed

    }
    else
    {
        if (log)
        {
            log->Printf ("SectionLoadList::%s (section = %p (%s), load_addr = 0x%16.16" PRIx64 ") error: module has been deleted",
                         __FUNCTION__,
                         section.get(),
                         section->GetName().AsCString(),
                         load_addr);
        }
    }
    return false;
}

size_t
SectionLoadList::SetSectionUnloaded (const lldb::SectionSP &section_sp)
{
    size_t unload_count = 0;

    if (section_sp)
    {
        Log *log(lldb_private::GetLogIfAnyCategoriesSet (LIBLLDB_LOG_DYNAMIC_LOADER | LIBLLDB_LOG_VERBOSE));

        if (log)
        {
            const FileSpec &module_file_spec (section_sp->GetModule()->GetFileSpec());
            log->Printf ("SectionLoadList::%s (section = %p (%s.%s))",
                         __FUNCTION__,
                         section_sp.get(),
                         module_file_spec.GetPath().c_str(),
                         section_sp->GetName().AsCString());
        }

        Mutex::Locker locker(m_mutex);
        
        sect_to_addr_collection::iterator sta_pos = m_sect_to_addr.find(section_sp.get());
        if (sta_pos != m_sect_to_addr.end())
        {
            ++unload_count;
            addr_t load_addr = sta_pos->second;
            m_sect_to_addr.erase (sta_pos);

            addr_to_sect_collection::iterator ats_pos = m_addr_to_sect.find(load_addr);
            if (ats_pos != m_addr_to_sect.end())
                m_addr_to_sect.erase (ats_pos);
        }
    }
    return unload_count;
}

bool
SectionLoadList::SetSectionUnloaded (const lldb::SectionSP &section_sp, addr_t load_addr)
{
    Log *log(lldb_private::GetLogIfAnyCategoriesSet (LIBLLDB_LOG_DYNAMIC_LOADER | LIBLLDB_LOG_VERBOSE));

    if (log)
    {
        const FileSpec &module_file_spec (section_sp->GetModule()->GetFileSpec());
        log->Printf ("SectionLoadList::%s (section = %p (%s.%s), load_addr = 0x%16.16" PRIx64 ")",
                     __FUNCTION__,
                     section_sp.get(),
                     module_file_spec.GetPath().c_str(),
                     section_sp->GetName().AsCString(),
                     load_addr);
    }
    bool erased = false;
    Mutex::Locker locker(m_mutex);
    sect_to_addr_collection::iterator sta_pos = m_sect_to_addr.find(section_sp.get());
    if (sta_pos != m_sect_to_addr.end())
    {
        erased = true;
        m_sect_to_addr.erase (sta_pos);
    }
        
    addr_to_sect_collection::iterator ats_pos = m_addr_to_sect.find(load_addr);
    if (ats_pos != m_addr_to_sect.end())
    {
        erased = true;
        m_addr_to_sect.erase (ats_pos);
    }

    return erased;
}


bool
SectionLoadList::ResolveLoadAddress (addr_t load_addr, Address &so_addr) const
{
    // First find the top level section that this load address exists in    
    Mutex::Locker locker(m_mutex);
    if (!m_addr_to_sect.empty())
    {
        addr_to_sect_collection::const_iterator pos = m_addr_to_sect.lower_bound (load_addr);
        if (pos != m_addr_to_sect.end())
        {
            if (load_addr != pos->first && pos != m_addr_to_sect.begin())
                --pos;
            const addr_t pos_load_addr = pos->first;
            if (load_addr >= pos_load_addr)
            {
                addr_t offset = load_addr - pos_load_addr;
                if (offset < pos->second->GetByteSize())
                {
                    // We have found the top level section, now we need to find the
                    // deepest child section.
                    return pos->second->ResolveContainedAddress (offset, so_addr);
                }
            }
        }
        else
        {
            // There are no entries that have an address that is >= load_addr,
            // so we need to check the last entry on our collection.
            addr_to_sect_collection::const_reverse_iterator rpos = m_addr_to_sect.rbegin();
            if (load_addr >= rpos->first)
            {
                addr_t offset = load_addr - rpos->first;
                if (offset < rpos->second->GetByteSize())
                {
                    // We have found the top level section, now we need to find the
                    // deepest child section.
                    return rpos->second->ResolveContainedAddress (offset, so_addr);
                }
            }
        }
    }
    so_addr.Clear();
    return false;
}

void
SectionLoadList::Dump (Stream &s, Target *target)
{
    Mutex::Locker locker(m_mutex);
    addr_to_sect_collection::const_iterator pos, end;
    for (pos = m_addr_to_sect.begin(), end = m_addr_to_sect.end(); pos != end; ++pos)
    {
        s.Printf("addr = 0x%16.16" PRIx64 ", section = %p: ", pos->first, pos->second.get());
        pos->second->Dump (&s, target, 0);
    }
}


