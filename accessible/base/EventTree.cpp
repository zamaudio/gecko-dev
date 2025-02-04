/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "EventTree.h"

#include "Accessible-inl.h"
#include "nsEventShell.h"
#include "DocAccessible.h"
#ifdef A11Y_LOG
#include "Logging.h"
#endif

using namespace mozilla;
using namespace mozilla::a11y;

////////////////////////////////////////////////////////////////////////////////
// TreeMutation class

EventTree* const TreeMutation::kNoEventTree = reinterpret_cast<EventTree*>(-1);

TreeMutation::TreeMutation(Accessible* aParent, bool aNoEvents) :
  mParent(aParent), mStartIdx(UINT32_MAX),
  mStateFlagsCopy(mParent->mStateFlags),
  mEventTree(aNoEvents ? kNoEventTree : nullptr)
{
#ifdef DEBUG
  mIsDone = false;
#endif

#ifdef A11Y_LOG
  if (mEventTree != kNoEventTree && logging::IsEnabled(logging::eEventTree)) {
    logging::MsgBegin("EVENTS_TREE", "reordering tree before");
    logging::AccessibleInfo("reordering for", mParent);
    Controller()->RootEventTree().Log();
    logging::MsgEnd();

    logging::MsgBegin("EVENTS_TREE", "Container tree");
    if (logging::IsEnabled(logging::eVerbose)) {
      nsAutoString level;
      Accessible* root = mParent->Document();
      do {
        const char* prefix = "";
        if (mParent == root) {
          prefix = "_X_";
        }
        else {
          const EventTree& ret = Controller()->RootEventTree();
          if (ret.Find(root)) {
            prefix = "_с_";
          }
        }

        printf("%s", NS_ConvertUTF16toUTF8(level).get());
        logging::AccessibleInfo(prefix, root);
        if (root->FirstChild() && !root->FirstChild()->IsDoc()) {
          level.Append(NS_LITERAL_STRING("  "));
          root = root->FirstChild();
          continue;
        }
        int32_t idxInParent = root->mParent ?
          root->mParent->mChildren.IndexOf(root) : -1;
        if (idxInParent != -1 &&
            idxInParent < static_cast<int32_t>(root->mParent->mChildren.Length() - 1)) {
          root = root->mParent->mChildren.ElementAt(idxInParent + 1);
          continue;
        }

        while ((root = root->Parent()) && !root->IsDoc()) {
          level.Cut(0, 2);

          int32_t idxInParent = root->mParent ?
          root->mParent->mChildren.IndexOf(root) : -1;
          if (idxInParent != -1 &&
              idxInParent < static_cast<int32_t>(root->mParent->mChildren.Length() - 1)) {
            root = root->mParent->mChildren.ElementAt(idxInParent + 1);
            break;
          }
        }
      }
      while (root && !root->IsDoc());
    }
    logging::MsgEnd();
  }
#endif

  mParent->mStateFlags |= Accessible::eKidsMutating;
}

TreeMutation::~TreeMutation()
{
  MOZ_ASSERT(mIsDone, "Done() must be called explicitly");
}

void
TreeMutation::AfterInsertion(Accessible* aChild)
{
  MOZ_ASSERT(aChild->Parent() == mParent);

  if (static_cast<uint32_t>(aChild->mIndexInParent) < mStartIdx) {
    mStartIdx = aChild->mIndexInParent + 1;
  }

  if (!mEventTree) {
    mEventTree = Controller()->QueueMutation(mParent);
    if (!mEventTree) {
      mEventTree = kNoEventTree;
    }
  }

  if (mEventTree != kNoEventTree) {
    mEventTree->Shown(aChild);
    Controller()->QueueNameChange(aChild);
  }
}

void
TreeMutation::BeforeRemoval(Accessible* aChild, bool aNoShutdown)
{
  MOZ_ASSERT(aChild->Parent() == mParent);

  if (static_cast<uint32_t>(aChild->mIndexInParent) < mStartIdx) {
    mStartIdx = aChild->mIndexInParent;
  }

  if (!mEventTree) {
    mEventTree = Controller()->QueueMutation(mParent);
    if (!mEventTree) {
      mEventTree = kNoEventTree;
    }
  }

  if (mEventTree != kNoEventTree) {
    mEventTree->Hidden(aChild, !aNoShutdown);
    Controller()->QueueNameChange(aChild);
  }
}

void
TreeMutation::Done()
{
  MOZ_ASSERT(mParent->mStateFlags & Accessible::eKidsMutating);
  mParent->mStateFlags &= ~Accessible::eKidsMutating;

  uint32_t length = mParent->mChildren.Length();
#ifdef DEBUG
  for (uint32_t idx = 0; idx < mStartIdx && idx < length; idx++) {
    MOZ_ASSERT(mParent->mChildren[idx]->mIndexInParent == static_cast<int32_t>(idx),
               "Wrong index detected");
  }
#endif

  for (uint32_t idx = mStartIdx; idx < length; idx++) {
    mParent->mChildren[idx]->mIndexInParent = idx;
    mParent->mChildren[idx]->mStateFlags |= Accessible::eGroupInfoDirty;
  }

  if (mStartIdx < mParent->mChildren.Length() - 1) {
    mParent->mEmbeddedObjCollector = nullptr;
  }

  mParent->mStateFlags |= mStateFlagsCopy & Accessible::eKidsMutating;

#ifdef DEBUG
  mIsDone = true;
#endif

#ifdef A11Y_LOG
  if (mEventTree != kNoEventTree && logging::IsEnabled(logging::eEventTree)) {
    logging::MsgBegin("EVENTS_TREE", "reordering tree after");
    logging::AccessibleInfo("reordering for", mParent);
    Controller()->RootEventTree().Log();
    logging::MsgEnd();
  }
#endif
}


////////////////////////////////////////////////////////////////////////////////
// EventTree

void
EventTree::Process()
{
  EventTree* node = mFirst;
  while (node) {
    node->Process();
    node = node->mNext;
  }

  // Fire mutation events.
  if (mContainer) {
    uint32_t eventsCount = mDependentEvents.Length();
    for (uint32_t jdx = 0; jdx < eventsCount; jdx++) {
      AccMutationEvent* mtEvent = mDependentEvents[jdx];
      MOZ_ASSERT(mtEvent->mEventRule != AccEvent::eDoNotEmit,
                 "The event shouldn't be presented in the tree");

      nsEventShell::FireEvent(mtEvent);
      if (mtEvent->mTextChangeEvent) {
        nsEventShell::FireEvent(mtEvent->mTextChangeEvent);
      }

      if (mtEvent->IsHide()) {
        // Fire menupopup end event before a hide event if a menu goes away.

        // XXX: We don't look into children of hidden subtree to find hiding
        // menupopup (as we did prior bug 570275) because we don't do that when
        // menu is showing (and that's impossible until bug 606924 is fixed).
        // Nevertheless we should do this at least because layout coalesces
        // the changes before our processing and we may miss some menupopup
        // events. Now we just want to be consistent in content insertion/removal
        // handling.
        if (mtEvent->mAccessible->ARIARole() == roles::MENUPOPUP) {
          nsEventShell::FireEvent(nsIAccessibleEvent::EVENT_MENUPOPUP_END,
                                  mtEvent->mAccessible);
        }

        AccHideEvent* hideEvent = downcast_accEvent(mtEvent);
        if (hideEvent->NeedsShutdown()) {
          mtEvent->GetDocAccessible()->ShutdownChildrenInSubtree(mtEvent->mAccessible);
        }
      }
    }

    // Fire reorder event at last.
    if (mFireReorder) {
      nsEventShell::FireEvent(nsIAccessibleEvent::EVENT_REORDER, mContainer);
    }

    mDependentEvents.Clear();
  }
}

EventTree*
EventTree::FindOrInsert(Accessible* aContainer)
{
  if (!mFirst) {
    return mFirst = new EventTree(aContainer);
  }

  EventTree* prevNode = nullptr;
  EventTree* node = mFirst;
  do {
    MOZ_ASSERT(!node->mContainer->IsApplication(),
               "No event for application accessible is expected here");
    MOZ_ASSERT(!node->mContainer->IsDefunct(), "An event target has to be alive");

    // Case of same target.
    if (node->mContainer == aContainer) {
      return node;
    }

    // Check if the given container is contained by a current node
    Accessible* tailRoot = aContainer->Document();
    Accessible* tailParent = aContainer;

    EventTree* matchNode = nullptr;
    Accessible* matchParent = nullptr;
    while (true) {
      // Reached a top, no match for a current event.
      if (tailParent == tailRoot) {
        // If we have a match in parents then continue to look in siblings.
        if (matchNode && node->mNext) {
          node = node->mNext;
          if (node->mContainer == aContainer) {
            return node; // case of same target
          }
          tailParent = aContainer;
          continue;
        }
        break;
      }

      // We got a match.
      if (tailParent->Parent() == node->mContainer) {
        matchNode = node;
        matchParent = tailParent;

        // Search the subtree for a better match.
        if (node->mFirst) {
          tailRoot = node->mContainer;
          node = node->mFirst;
          if (node->mContainer == aContainer) {
            return node; // case of same target
          }
          tailParent = aContainer;
          continue;
        }
        break;
      }

      tailParent = tailParent->Parent();
      MOZ_ASSERT(tailParent, "Wrong tree");
      if (!tailParent) {
        break;
      }
    }

    // The given node is contained by a current node
    //   if hide of a current node contains the given node
    //   then assert
    //   if show of a current node contains the given node
    //   then ignore the given node
    //   otherwise ignore the given node, but not its show and hide events
    if (matchNode) {
      uint32_t eventType = 0;
      uint32_t count = matchNode->mDependentEvents.Length();
      for (uint32_t idx = count - 1; idx < count; idx--) {
        if (matchNode->mDependentEvents[idx]->mAccessible == matchParent) {
          eventType = matchNode->mDependentEvents[idx]->mEventType;
        }
      }
      MOZ_ASSERT(eventType != nsIAccessibleEvent::EVENT_HIDE,
                 "Accessible tree was modified after it was removed");

      // If contained by show event target then no events are required.
      if (eventType == nsIAccessibleEvent::EVENT_SHOW) {
        return nullptr;
      }

      node->mFirst = new EventTree(aContainer);
      node->mFirst->mFireReorder = false;
      return node->mFirst;
    }

    // If the given node contains a current node
    // then
    //   if show or hide of the given node contains a grand parent of the current node
    //   then ignore the current node and its show and hide events
    //   otherwise ignore the current node, but not its show and hide events
    Accessible* curParent = node->mContainer;
    while (curParent && !curParent->IsDoc()) {
      if (curParent->Parent() != aContainer) {
        curParent = curParent->Parent();
        continue;
      }

      // Insert the tail node into the hierarchy between the current node and
      // its parent.
      node->mFireReorder = false;
      nsAutoPtr<EventTree>& nodeOwnerRef = prevNode ? prevNode->mNext : mFirst;
      nsAutoPtr<EventTree> newNode(new EventTree(aContainer));
      newNode->mFirst = Move(nodeOwnerRef);
      nodeOwnerRef = Move(newNode);
      nodeOwnerRef->mNext = Move(node->mNext);

      // Check if a next node is contained by the given node too, and move them
      // under the given node if so.
      prevNode = nodeOwnerRef;
      node = nodeOwnerRef->mNext;
      nsAutoPtr<EventTree>* nodeRef = &nodeOwnerRef->mNext;
      EventTree* insNode = nodeOwnerRef->mFirst;
      while (node) {
        Accessible* curParent = node->mContainer;
        while (curParent && !curParent->IsDoc()) {
          if (curParent->Parent() != aContainer) {
            curParent = curParent->Parent();
            continue;
          }

          MOZ_ASSERT(!insNode->mNext);

          node->mFireReorder = false;
          insNode->mNext = Move(*nodeRef);
          insNode = insNode->mNext;

          prevNode->mNext = Move(node->mNext);
          node = prevNode;
          break;
        }

        prevNode = node;
        nodeRef = &node->mNext;
        node = node->mNext;
      }

      return nodeOwnerRef;
    }

    prevNode = node;
  } while ((node = node->mNext));

  MOZ_ASSERT(prevNode, "Nowhere to insert");
  return prevNode->mNext = new EventTree(aContainer);
}

void
EventTree::Clear()
{
  mFirst = nullptr;
  mNext = nullptr;
  mContainer = nullptr;

  uint32_t eventsCount = mDependentEvents.Length();
  for (uint32_t jdx = 0; jdx < eventsCount; jdx++) {
    AccHideEvent* ev = downcast_accEvent(mDependentEvents[jdx]);
    if (ev && ev->NeedsShutdown()) {
      ev->GetDocAccessible()->ShutdownChildrenInSubtree(ev->mAccessible);
    }
  }
  mDependentEvents.Clear();
}

const EventTree*
EventTree::Find(const Accessible* aContainer) const
{
  const EventTree* et = this;
  while (et) {
    if (et->mContainer == aContainer) {
      return et;
    }

    if (et->mFirst) {
      et = et->mFirst;
      const EventTree* cet = et->Find(aContainer);
      if (cet) {
        return cet;
      }
    }

    et = et->mNext;
    const EventTree* cet = et->Find(aContainer);
    if (cet) {
      return cet;
    }
  }

  return nullptr;
}

#ifdef A11Y_LOG
void
EventTree::Log(uint32_t aLevel) const
{
  if (aLevel == UINT32_MAX) {
    if (mFirst) {
      mFirst->Log(0);
    }
    return;
  }

  for (uint32_t i = 0; i < aLevel; i++) {
    printf("  ");
  }
  logging::AccessibleInfo("container", mContainer);

  for (uint32_t i = 0; i < mDependentEvents.Length(); i++) {
    AccMutationEvent* ev = mDependentEvents[i];
    if (ev->IsShow()) {
      for (uint32_t i = 0; i < aLevel; i++) {
        printf("  ");
      }
      logging::AccessibleInfo("shown", ev->mAccessible);
    }
    else {
      for (uint32_t i = 0; i < aLevel; i++) {
        printf("  ");
      }
      logging::AccessibleInfo("hidden", ev->mAccessible);
    }
  }

  if (mFirst) {
    mFirst->Log(aLevel + 1);
  }

  if (mNext) {
    mNext->Log(aLevel);
  }
}
#endif

void
EventTree::Mutated(AccMutationEvent* aEv)
{
  // If shown or hidden node is a root of previously mutated subtree, then
  // discard those subtree mutations as we are no longer interested in them.
  nsAutoPtr<EventTree>* node = &mFirst;
  while (*node) {
    if ((*node)->mContainer == aEv->mAccessible) {
      *node = Move((*node)->mNext);
      break;
    }
    node = &(*node)->mNext;
  }

  AccMutationEvent* prevEvent = mDependentEvents.SafeLastElement(nullptr);
  mDependentEvents.AppendElement(aEv);

  // Coalesce text change events from this hide/show event and the previous one.
  if (prevEvent && aEv->mEventType == prevEvent->mEventType) {
    if (aEv->IsHide()) {
      // XXX: we need a way to ignore SplitNode and JoinNode() when they do not
      // affect the text within the hypertext.
      AccTextChangeEvent* prevTextEvent = prevEvent->mTextChangeEvent;
      if (prevTextEvent) {
        AccHideEvent* hideEvent = downcast_accEvent(aEv);
        AccHideEvent* prevHideEvent = downcast_accEvent(prevEvent);

        if (prevHideEvent->mNextSibling == hideEvent->mAccessible) {
          hideEvent->mAccessible->AppendTextTo(prevTextEvent->mModifiedText);
        }
        else if (prevHideEvent->mPrevSibling == hideEvent->mAccessible) {
          uint32_t oldLen = prevTextEvent->GetLength();
          hideEvent->mAccessible->AppendTextTo(prevTextEvent->mModifiedText);
          prevTextEvent->mStart -= prevTextEvent->GetLength() - oldLen;
        }

        hideEvent->mTextChangeEvent.swap(prevEvent->mTextChangeEvent);
      }
    }
    else {
      AccTextChangeEvent* prevTextEvent = prevEvent->mTextChangeEvent;
      if (prevTextEvent) {
        if (aEv->mAccessible->IndexInParent() ==
            prevEvent->mAccessible->IndexInParent() + 1) {
          // If tail target was inserted after this target, i.e. tail target is next
          // sibling of this target.
          aEv->mAccessible->AppendTextTo(prevTextEvent->mModifiedText);
        }
        else if (aEv->mAccessible->IndexInParent() ==
                 prevEvent->mAccessible->IndexInParent() - 1) {
          // If tail target was inserted before this target, i.e. tail target is
          // previous sibling of this target.
          nsAutoString startText;
          aEv->mAccessible->AppendTextTo(startText);
          prevTextEvent->mModifiedText = startText + prevTextEvent->mModifiedText;
          prevTextEvent->mStart -= startText.Length();
        }

        aEv->mTextChangeEvent.swap(prevEvent->mTextChangeEvent);
      }
    }
  }

  // Create a text change event caused by this hide/show event. When a node is
  // hidden/removed or shown/appended, the text in an ancestor hyper text will
  // lose or get new characters.
  if (aEv->mTextChangeEvent || !mContainer->IsHyperText()) {
    return;
  }

  nsAutoString text;
  aEv->mAccessible->AppendTextTo(text);
  if (text.IsEmpty()) {
    return;
  }

  int32_t offset = mContainer->AsHyperText()->GetChildOffset(aEv->mAccessible);
  aEv->mTextChangeEvent =
    new AccTextChangeEvent(mContainer, offset, text, aEv->IsShow(),
                           aEv->mIsFromUserInput ? eFromUserInput : eNoUserInput);
}
