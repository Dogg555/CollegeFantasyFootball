(function initFreeAgentDirectory(root) {
  'use strict';

  const STARTER_SLOTS = ['qb', 'rb', 'wr', 'te', 'flex', 'k', 'def'];
  const ALL_SLOTS = [...STARTER_SLOTS, 'bench'];

  function numberRule(rules, key, fallback = 0) {
    const value = Number(rules?.[key]);
    return Number.isFinite(value) && value >= 0 ? Math.floor(value) : fallback;
  }

  function rosterLimit(rules = {}) {
    return ALL_SLOTS.reduce((total, slot) => total + numberRule(rules, slot, slot === 'bench' ? 6 : 0), 0);
  }

  function normalizedPosition(player = {}) {
    const value = String(player.position || '').trim().toLowerCase();
    return value === 'dst' ? 'def' : value;
  }

  function flexEligible(position) {
    return ['rb', 'wr', 'te'].includes(String(position || '').toLowerCase());
  }

  function playerPoolEligible(player = {}, rules = {}) {
    const position = normalizedPosition(player);
    if (flexEligible(position)) return numberRule(rules, position) > 0 || numberRule(rules, 'flex') > 0;
    if (['qb', 'k', 'def'].includes(position)) return numberRule(rules, position) > 0;
    return false;
  }

  function slotCounts(roster = [], excludingPlayerId = '') {
    const counts = {};
    roster.forEach((player) => {
      if (String(player?.id || player?.playerId || '') === String(excludingPlayerId || '')) return;
      const slot = String(player?.rosterSlot || 'bench').trim().toLowerCase() || 'bench';
      counts[slot] = (counts[slot] || 0) + 1;
    });
    return counts;
  }

  function destinationSlot(player = {}, roster = [], rules = {}, excludingPlayerId = '') {
    if (!playerPoolEligible(player, rules)) return '';
    const counts = slotCounts(roster, excludingPlayerId);
    const position = normalizedPosition(player);
    if (numberRule(rules, position) > 0 && (counts[position] || 0) < numberRule(rules, position)) return position;
    if (flexEligible(position) && (counts.flex || 0) < numberRule(rules, 'flex')) return 'flex';
    if ((counts.bench || 0) < numberRule(rules, 'bench', 6)) return 'bench';
    return '';
  }

  function requiresDrop(roster = [], rules = {}) {
    return roster.length >= rosterLimit(rules);
  }

  function lockMap(locks = []) {
    return new Map((Array.isArray(locks) ? locks : []).map((lock) => [String(lock?.playerId || ''), Boolean(lock?.locked)]));
  }

  function eligibleDropCandidates(addPlayer = {}, roster = [], rules = {}, locks = []) {
    const locked = lockMap(locks);
    return roster.filter((player) => {
      const id = String(player?.id || player?.playerId || '');
      return id && !locked.get(id) && Boolean(destinationSlot(addPlayer, roster, rules, id));
    });
  }

  function availabilityAction(player = {}, hasLeague = true) {
    if (!hasLeague) return { label: 'Select a league', enabled: false, action: 'none' };
    const state = String(player.availability || 'available').toLowerCase();
    const actions = {
      available: { label: 'Add', enabled: true, action: 'add' },
      rostered: { label: 'On your roster', enabled: false, action: 'none' },
      owned: { label: 'Rostered', enabled: false, action: 'none' },
      waivers: { label: 'Waivers', enabled: false, action: 'waiver' },
      locked: { label: 'Locked', enabled: false, action: 'none' },
      ineligible: { label: 'Ineligible', enabled: false, action: 'none' },
      injured_reserve: { label: 'IR', enabled: false, action: 'none' },
      suspended: { label: 'Suspended', enabled: false, action: 'none' },
      drafted: { label: 'Drafted', enabled: false, action: 'none' },
      released: { label: 'Released', enabled: false, action: 'none' }
    };
    return actions[state] || { label: 'Unavailable', enabled: false, action: 'none' };
  }

  function buildRosterPreview(addPlayer = {}, roster = [], rules = {}, dropPlayerId = '') {
    const dropId = String(dropPlayerId || '');
    const drop = roster.find((player) => String(player?.id || player?.playerId || '') === dropId) || null;
    const destination = destinationSlot(addPlayer, roster, rules, dropId);
    const resultingRoster = roster
      .filter((player) => String(player?.id || player?.playerId || '') !== dropId)
      .concat(destination ? [{ ...addPlayer, rosterSlot: destination }] : []);
    return {
      requiresDrop: requiresDrop(roster, rules),
      drop,
      destination,
      valid: Boolean(destination) && (!requiresDrop(roster, rules) || Boolean(drop)),
      rosterCountBefore: roster.length,
      rosterCountAfter: resultingRoster.length,
      rosterLimit: rosterLimit(rules),
      resultingRoster
    };
  }

  const api = Object.freeze({
    STARTER_SLOTS,
    rosterLimit,
    normalizedPosition,
    flexEligible,
    playerPoolEligible,
    slotCounts,
    destinationSlot,
    requiresDrop,
    eligibleDropCandidates,
    availabilityAction,
    buildRosterPreview
  });

  root.CFFFreeAgentDirectory = api;
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
})(typeof window !== 'undefined' ? window : globalThis);
