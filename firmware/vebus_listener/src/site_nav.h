#pragma once

#define BRAIN_ZERO_LOCALE_SCRIPT \
    "<script src='/ui/language.js' defer></script><script src='/ui/translate.js' defer></script>"

// Shared page fragment. Native disclosure controls work without JavaScript;
// the script marks the current route and handles dismissal.
#define BRAIN_ZERO_NAV \
    "<nav class='site-nav' aria-label='Main navigation'>" \
    "<a class='site-brand' href='/' aria-label='brain_zero dashboard'><picture>" \
    "<source media='(prefers-color-scheme: dark)' srcset='/brand/brain-zero-dark.svg'>" \
    "<img src='/brand/brain-zero.svg' alt='brain_zero' width='1200' height='320'></picture></a>" \
    "<div class='site-nav-links'>" \
    "<a href='/'>Overview</a><a href='/inverter'>Inverter</a><a href='/ess'>ESS</a><a href='/settings'>Settings</a>" \
    "<details class='site-nav-group' name='brain-zero-nav'><summary>System</summary>" \
    "<div class='site-nav-menu'><a href='/multi'>Inverter settings</a>" \
    "<a href='/diagnostics'>Diagnostics</a>" \
    "<a href='/firmware'>Firmware update</a></div></details>" \
    "</div></nav>" \
    "<script>(()=>{" \
    "const nav=document.currentScript.previousElementSibling;" \
    "for(const link of nav.querySelectorAll('.site-nav-links a'))" \
    "if(link.pathname===location.pathname)link.setAttribute('aria-current','page');" \
    "else if(link.pathname==='/settings'&&location.pathname.startsWith('/settings/'))link.setAttribute('aria-current','location');" \
    "const groups=nav.querySelectorAll('details');" \
    "for(const group of groups)group.addEventListener('toggle',()=>{" \
    "if(group.open)for(const other of groups)if(other!==group)other.open=false});" \
    "document.addEventListener('click',e=>{for(const group of groups)" \
    "if(group.open&&!group.contains(e.target))group.open=false});" \
    "nav.addEventListener('keydown',e=>{if(e.key!=='Escape')return;" \
    "const group=nav.querySelector('details[open]');" \
    "if(group){group.open=false;group.querySelector('summary').focus();e.preventDefault()}});" \
    "})();</script>" BRAIN_ZERO_LOCALE_SCRIPT
