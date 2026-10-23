/* GCC 14.2 Win32: require the unused thread-exit facilities to be absent.
 * No dependency source/archive is modified. Companion --wrap flags with NO
 * wrapper definitions must reject every external call/address-taking reference
 * to all removed functions. /DISCARD/ alone is insufficient on GNU PE ld.
 * The exact reviewed archive has no intra-object caller outside these roots;
 * its byte identity and this property are qualification preconditions.
 * Preserve ordinary condition_variable members needed by shared_mutex/clvk.
 * Tests must prove positive linking plus direct/address-taking rejections.
 */
SECTIONS
{
  /DISCARD/ :
  {
    *(.text$_ZSt16__at_thread_exitPSt20__at_thread_exit_elt)
    *(.pdata$_ZSt16__at_thread_exitPSt20__at_thread_exit_elt)
    *(.xdata$_ZSt16__at_thread_exitPSt20__at_thread_exit_elt)
    *(.text$_ZSt25notify_all_at_thread_exitRSt18condition_variableSt11unique_lockISt5mutexE)
    *(.pdata$_ZSt25notify_all_at_thread_exitRSt18condition_variableSt11unique_lockISt5mutexE)
    *(.xdata$_ZSt25notify_all_at_thread_exitRSt18condition_variableSt11unique_lockISt5mutexE)
    *(.data$_ZZSt16__at_thread_exitPSt20__at_thread_exit_eltE4once)
    *(.text$_ZNSt13__future_base13_State_baseV211_Make_ready6_M_setEv)
    *(.pdata$_ZNSt13__future_base13_State_baseV211_Make_ready6_M_setEv)
    *(.xdata$_ZNSt13__future_base13_State_baseV211_Make_ready6_M_setEv)
  }
}
INSERT AFTER .text;
