//! A map that iterates in insertion order, as a Python dict does: setting a
//! key already there keeps its place; removing one drops it from the order.

use std::collections::HashMap;
use std::hash::Hash;

#[derive(Clone, Debug)]
pub struct OMap<K: Eq + Hash + Clone, V> {
    order: Vec<K>,
    map: HashMap<K, V>,
}

impl<K: Eq + Hash + Clone, V> Default for OMap<K, V> {
    fn default() -> Self {
        OMap { order: Vec::new(), map: HashMap::new() }
    }
}

impl<K: Eq + Hash + Clone, V> OMap<K, V> {
    pub fn new() -> Self {
        Self::default()
    }
    pub fn insert(&mut self, k: K, v: V) {
        if self.map.insert(k.clone(), v).is_none() {
            self.order.push(k);
        }
    }
    pub fn get(&self, k: &K) -> Option<&V> {
        self.map.get(k)
    }
    pub fn get_mut(&mut self, k: &K) -> Option<&mut V> {
        self.map.get_mut(k)
    }
    pub fn contains_key(&self, k: &K) -> bool {
        self.map.contains_key(k)
    }
    pub fn len(&self) -> usize {
        self.map.len()
    }
    pub fn is_empty(&self) -> bool {
        self.map.is_empty()
    }
    pub fn keys(&self) -> impl Iterator<Item = &K> + '_ {
        self.order.iter()
    }
    pub fn values(&self) -> impl Iterator<Item = &V> + '_ {
        self.order.iter().map(move |k| &self.map[k])
    }
    pub fn iter(&self) -> impl Iterator<Item = (&K, &V)> + '_ {
        self.order.iter().map(move |k| (k, &self.map[k]))
    }
    /// Keeps the keys `keep` says yes to, in their order.
    pub fn retain(&mut self, mut keep: impl FnMut(&K) -> bool) {
        let map = &mut self.map;
        self.order.retain(|k| {
            if keep(k) {
                true
            } else {
                map.remove(k);
                false
            }
        });
    }
}

impl<K: Eq + Hash + Clone, V> std::ops::Index<&K> for OMap<K, V> {
    type Output = V;
    fn index(&self, k: &K) -> &V {
        &self.map[k]
    }
}
